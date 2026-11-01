/**
 * @file mode_manager.c
 * @brief Mode state machine and the real-time loop.
 */
#include "mode_manager.h"

#include <stdio.h>
#include <string.h>
#include "hw_common.h"
#include "error_handler.h"
#include "data_acquisition.h"
#include "pipeline.h"
#include "light_control.h"
#include "led_patterns.h"
#include "fw_updater.h"
#include "self_diag.h"
#include "calibration.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "mode";

#define MIC_TIMEOUT_MS      50u
#define HOP_US              ((uint32_t)AUDIO_HOP_SIZE * 1000000u / SAMPLE_RATE)

static volatile dev_mode_t s_mode = DEV_MODE_MAIN;
static volatile uint8_t s_run_mode;
static volatile uint8_t s_pattern;
static volatile bool    s_fw_ack;
static bool             s_mic_ok;

static audio_frame_t    s_frame;
static live_snapshot_t  s_live;
static portMUX_TYPE     s_live_lock = portMUX_INITIALIZER_UNLOCKED;

static rt_stats_t       s_stats;
static uint64_t         s_proc_sum, s_lat_sum;

#ifdef ENABLE_SELF_DIAGNOSTICS
static diag_bench_t s_bench_block = DIAG_BENCH_INIT("audio", "block_total");
#endif

/* ---------------------------------------------------------------------------- */

dev_mode_t mode_get(void)
{
    return s_mode;
}

int mode_set(dev_mode_t m)
{
    if (s_mode == DEV_MODE_FW_UPDATE || m == DEV_MODE_FW_UPDATE) return HW_ERR_STATE;
    if (m == s_mode) return HW_OK;
    if (s_mode == DEV_MODE_CALIBRATION && m == DEV_MODE_MAIN) {
        calib_reload();             /* discard unsaved changes */
    }
    s_mode = m;
    ESP_LOGI(TAG, "device mode -> %s", m == DEV_MODE_CALIBRATION ? "CALIBRATION" : "MAIN");
    return HW_OK;
}

int mode_enter_fw_update(void)
{
    if (s_mode == DEV_MODE_CALIBRATION) calib_reload();
    s_fw_ack = false;
    s_mode = DEV_MODE_FW_UPDATE;
    for (int i = 0; i < 100 && !s_fw_ack; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "device mode -> FW_UPDATE");
    return s_fw_ack ? HW_OK : HW_ERR_TIMEOUT;
}

void mode_exit_fw_update(void)
{
    if (s_mode == DEV_MODE_FW_UPDATE) {
        s_mode = DEV_MODE_MAIN;
        ESP_LOGI(TAG, "device mode -> MAIN");
    }
}

void mode_apply_run_config(uint8_t run_mode, uint8_t algorithm, uint8_t pattern)
{
    s_run_mode = run_mode;
    s_pattern = pattern;
    if (algorithm != pipeline_active()) {
        pipeline_select(algorithm);
    }
}

run_mode_t mode_run_mode(void)
{
    return (run_mode_t)s_run_mode;
}

uint8_t mode_pattern(void)
{
    return s_pattern;
}

void mode_get_live(live_snapshot_t *out)
{
    portENTER_CRITICAL(&s_live_lock);
    *out = s_live;
    portEXIT_CRITICAL(&s_live_lock);
}

void mode_get_rt_stats(rt_stats_t *out)
{
    *out = s_stats;
    out->proc_avg_us = s_stats.blocks ? (uint32_t)(s_proc_sum / s_stats.blocks) : 0;
    out->latency_avg_us = s_stats.blocks ? (uint32_t)(s_lat_sum / s_stats.blocks) : 0;
}

void mode_reset_rt_stats(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    s_proc_sum = 0;
    s_lat_sum = 0;
}

/* ---------------------------------------------------------------------------- */

static void publish_live(const audio_frame_t *f, bool audio)
{
    portENTER_CRITICAL(&s_live_lock);
    if (audio) {
        s_live.level = f->level;
        s_live.bpm_x10 = f->bpm_x10;
        s_live.beat_count = (uint16_t)f->beat_count;
        s_live.centroid = f->centroid;
        memcpy(s_live.bands, f->bands, sizeof(s_live.bands));
    } else {
        memset(&s_live, 0, sizeof(s_live));
    }
    portEXIT_CRITICAL(&s_live_lock);
}

static void prepare_frame(audio_frame_t *f)
{
    f->n = AUDIO_HOP_SIZE;
    f->block++;
    f->beat = false;
    f->beat_strength = 0;
    f->spectrum_valid = false;
    f->leds = light_framebuffer();
    f->num_leds = light_num_leds();
}

static void audio_block(void)
{
    static uint8_t timeouts;
    if (daq_read_block(s_frame.samples, MIC_TIMEOUT_MS) != HW_OK) {
        if (++timeouts == 3) err_report(ERR_MIC_TIMEOUT);
        light_show();                       /* keep error overlay alive */
        return;
    }
    if (timeouts >= 3) err_clear(ERR_MIC_TIMEOUT);
    timeouts = 0;

#ifdef ENABLE_SELF_DIAGNOSTICS
    diag_bench_begin(&s_bench_block);
#endif
    uint32_t t0 = hw_cycles();
    prepare_frame(&s_frame);
    pipeline_process(&s_frame);
    light_show();
    uint32_t t1 = hw_cycles();
#ifdef ENABLE_SELF_DIAGNOSTICS
    diag_bench_end(&s_bench_block);
#endif

    uint32_t proc_us = hw_cycles_to_us(t1 - t0);
    uint32_t lat_us = hw_cycles_to_us(t1 - daq_last_block_cycles());
    s_stats.blocks++;
    s_proc_sum += proc_us;
    s_lat_sum += lat_us;
    if (proc_us > s_stats.proc_max_us) s_stats.proc_max_us = proc_us;
    if (lat_us > s_stats.latency_max_us) s_stats.latency_max_us = lat_us;
    if (proc_us > HOP_US) {
        s_stats.overruns++;
        err_report(ERR_AUDIO_OVERRUN);
    }
    publish_live(&s_frame, true);
}

static void frame_tick(TickType_t *last)
{
    vTaskDelayUntil(last, pdMS_TO_TICKS(LED_PATTERN_FRAME_MS));
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);
    led_rgb_t *fb = light_framebuffer();
    uint16_t n = light_num_leds();
    if (s_mode == DEV_MODE_FW_UPDATE) {
        led_pattern_progress(fwu_progress(), t, fb, n);
    } else if (s_run_mode == RUN_MODE_PATTERN || !s_mic_ok) {
        led_pattern_render(s_pattern, t, fb, n);
    } else {
        memset(fb, 0, n * sizeof(led_rgb_t));
    }
    light_show();
    publish_live(NULL, false);
}

static void rt_task(void *arg)
{
    wdt_client_t wdt = wdt_register("rt", WDT_RT_TASK_TIMEOUT_MS);

    s_mic_ok = (daq_init(xTaskGetCurrentTaskHandle()) == HW_OK);

    /* start-up animation (also proves the strip works) */
    int64_t t_start = esp_timer_get_time();
    while (led_pattern_boot((uint32_t)((esp_timer_get_time() - t_start) / 1000), light_framebuffer(),
                            light_num_leds())) {
        light_show();
        wdt_checkin(wdt);
        vTaskDelay(pdMS_TO_TICKS(LED_PATTERN_FRAME_MS));
    }

#ifdef ENABLE_SELF_DIAGNOSTICS
    diag_bench_register(&s_bench_block);
#endif
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        wdt_checkin(wdt);
        bool want_audio = s_mic_ok && s_mode != DEV_MODE_FW_UPDATE && s_run_mode == RUN_MODE_AUDIO;
        if (want_audio && !daq_running()) {
            pipeline_select(pipeline_active());     /* restart stages with clean state */
            daq_start();
            ESP_LOGI(TAG, "audio started");
        } else if (!want_audio && daq_running()) {
            daq_stop();
            ESP_LOGI(TAG, "audio stopped");
            last = xTaskGetTickCount();
        }
        if (s_mode == DEV_MODE_FW_UPDATE) s_fw_ack = true;

        if (daq_running()) {
            audio_block();
        } else {
            frame_tick(&last);
        }
    }
}

#ifdef ENABLE_SELF_DIAGNOSTICS
static void diag_provider(diag_writer_t *w)
{
    rt_stats_t rs;
    mode_get_rt_stats(&rs);
    daq_stats_t ds;
    daq_get_stats(&ds);
    light_stats_t ls;
    light_get_stats(&ls);

    diag_write(w, "audio", "sample_rate", "Hz", "%lu", (unsigned long)ds.actual_rate);
    diag_write(w, "audio", "hop", "us", "%lu", (unsigned long)HOP_US);
    diag_write(w, "audio", "mic_channel", "", "%s%s", ds.channel == MIC_CHANNEL_LEFT ? "left" : "right",
               ds.auto_detecting ? " (detecting)" : "");
    diag_write(w, "audio", "mic_signal", "", "%s", ds.signal_ok ? "ok" : "NONE");
    diag_write(w, "audio", "blocks", "", "%lu", (unsigned long)ds.blocks);
    diag_write(w, "audio", "blocks_dropped", "", "%lu", (unsigned long)ds.dropped);
    diag_write(w, "audio", "mic_timeouts", "", "%lu", (unsigned long)ds.timeouts);
    diag_write(w, "audio", "processing_avg", "us", "%lu", (unsigned long)rs.proc_avg_us);
    diag_write(w, "audio", "processing_max", "us", "%lu", (unsigned long)rs.proc_max_us);
    diag_write(w, "audio", "overruns", "", "%lu", (unsigned long)rs.overruns);
    diag_write(w, "latency", "block_to_led_avg", "us", "%lu", (unsigned long)rs.latency_avg_us);
    diag_write(w, "latency", "block_to_led_max", "us", "%lu", (unsigned long)rs.latency_max_us);
    /* mic->light: mean sample age in a block (hop/2) + processing + LED wire time */
    uint32_t wire = ws2812_frame_time_us(LED_MAX_COUNT);
    diag_write(w, "latency", "mic_to_light_est", "us", "%lu",
               (unsigned long)(HOP_US / 2 + rs.latency_avg_us + wire));

    const algorithm_def_t *a = &g_algorithms[pipeline_active()];
    for (uint8_t i = 0; i < a->num_stages; i++) {
        uint32_t avg, max;
        pipeline_stage_timing(i, &avg, &max);
        char name[40];
        snprintf(name, sizeof(name), "%u:%s", i, a->stages[i]->name);
        diag_write(w, "stage", name, "us", "avg %lu.%02lu max %lu.%02lu",
                   (unsigned long)(avg / 160), (unsigned long)((avg % 160) * 100 / 160),
                   (unsigned long)(max / 160), (unsigned long)((max % 160) * 100 / 160));
    }
    diag_write(w, "leds", "frames", "", "%lu", (unsigned long)ls.frames);
    diag_write(w, "leds", "power_limited_frames", "", "%lu", (unsigned long)ls.limited_frames);
    diag_write(w, "leds", "est_current", "mA", "%lu", (unsigned long)ls.est_current_ma);
    diag_write(w, "leds", "wire_time", "us", "%lu", (unsigned long)wire);
}
#endif

void mode_init(uint8_t run_mode, uint8_t algorithm, uint8_t pattern)
{
    s_run_mode = run_mode;
    s_pattern = pattern;
    pipeline_init(algorithm);
#ifdef ENABLE_SELF_DIAGNOSTICS
    diag_register_provider(diag_provider);
#endif
    xTaskCreate(rt_task, "rt", TASK_STACK_RT, NULL, TASK_PRIO_RT, NULL);
}
