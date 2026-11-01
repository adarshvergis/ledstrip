/**
 * @file cli_commands.c
 * @brief Debug CLI commands (compiled only with ENABLE_DEBUG_CLI).
 */
#include "comm.h"
#include "build_config.h"

#ifdef ENABLE_DEBUG_CLI

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cli.h"
#include "hw_common.h"
#include "calibration.h"
#include "mode_manager.h"
#include "pipeline.h"
#include "light_control.h"
#include "led_patterns.h"
#include "data_acquisition.h"
#include "network.h"
#include "error_handler.h"
#include "fw_updater.h"
#include "self_diag.h"
#include "config_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#define ARG_INT(i, def) ((argc > (i)) ? (int)strtol(argv[i], NULL, 0) : (def))

static int c_version(int argc, char **argv, cli_out_t *o)
{
    cli_printf(o, "%s %s (built %s)\n", FW_PRODUCT_NAME, FW_VERSION_STRING, FW_BUILD_DATE_STRING);
    return 0;
}

static int c_info(int argc, char **argv, cli_out_t *o)
{
    static const char *modes[] = {"MAIN", "CALIBRATION", "FW_UPDATE"};
    static const char *net[] = {"off", "-", "station", "connecting"};
    const settings_t *s = settings_get();
    cli_printf(o, "fw        %s, running from %s\n", FW_VERSION_STRING, fwu_running_partition());
    cli_printf(o, "uptime    %llu s\n", (unsigned long long)(esp_timer_get_time() / 1000000));
    cli_printf(o, "mode      %s, run mode %s\n", modes[mode_get()], mode_run_mode() == RUN_MODE_AUDIO ? "audio" : "pattern");
    cli_printf(o, "algorithm %u '%s'\n", pipeline_active(), g_algorithms[pipeline_active()].name);
    cli_printf(o, "pattern   %u '%s'\n", mode_pattern(), led_pattern_name(mode_pattern()));
    cli_printf(o, "leds      %u, brightness %u, limit %u mA\n", light_num_leds(), light_brightness(), s->max_current_ma);
    cli_printf(o, "network   %s ip %s mac %s rssi %d\n", net[net_state()], net_ip_str(), net_mac_str(), net_rssi());
    cli_printf(o, "audio     %u Hz, hop %u, fft %u, bands %u\n", SAMPLE_RATE, AUDIO_HOP_SIZE, AUDIO_FFT_SIZE, AUDIO_NUM_BANDS);
    cli_printf(o, "heap      %lu B free\n", (unsigned long)esp_get_free_heap_size());
    return 0;
}

static int c_set_led_strip(int argc, char **argv, cli_out_t *o)
{
    if (argc < 4) return -1;
    uint32_t rgb = (uint32_t)strtoul(argv[1][0] == '#' ? argv[1] + 1 : argv[1], NULL, 16);
    int start = atoi(argv[2]);
    int end = atoi(argv[3]);
    int hold = ARG_INT(4, 10000);
    if (start < 0 || end < start) return -2;
    light_test_set((led_rgb_t){(uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb}, (uint16_t)start,
                   (uint16_t)end, (uint32_t)hold);
    cli_printf(o, "LEDs %d..%d = #%06lx for %d ms (0 = until led_release)\n", start, end, (unsigned long)rgb, hold);
    return 0;
}

static int c_led_release(int argc, char **argv, cli_out_t *o)
{
    light_test_release();
    cli_printf(o, "test override released\n");
    return 0;
}

static int capture_and_print(int8_t stage, int n, cli_out_t *o)
{
    if (mode_run_mode() != RUN_MODE_AUDIO || mode_get() == DEV_MODE_FW_UPDATE) {
        cli_printf(o, "audio is not running (run mode pattern / fw update)\n");
        return 0;
    }
    if (n < 1 || n > PIPELINE_CAPTURE_MAX) n = 64;
    int r = pipeline_capture_start(stage, (uint16_t)n);
    if (r == HW_ERR_BUSY) { cli_printf(o, "capture busy\n"); return 0; }
    if (r != HW_OK) return -2;
    for (int i = 0; i < 200 && !pipeline_capture_done(); i++) vTaskDelay(pdMS_TO_TICKS(5));
    if (!pipeline_capture_done()) { cli_printf(o, "capture timeout\n"); return 0; }
    uint16_t got;
    const q31_t *d = pipeline_capture_data(&got);
    cli_printf(o, "%u samples (Q15 scale = Q31 >> 16):\n", got);
    for (uint16_t i = 0; i < got; i++) {
        cli_printf(o, "%7ld%s", (long)(d[i] >> 16), ((i % 8) == 7 || i == got - 1) ? "\n" : " ");
    }
    return 0;
}

static int c_print_mic(int argc, char **argv, cli_out_t *o)
{
    return capture_and_print(PIPELINE_CAPTURE_INPUT, ARG_INT(1, 64), o);
}

static int c_print_processed(int argc, char **argv, cli_out_t *o)
{
    if (argc < 2) return -1;
    return capture_and_print((int8_t)atoi(argv[1]), ARG_INT(2, 64), o);
}

static int c_stages(int argc, char **argv, cli_out_t *o)
{
    const algorithm_def_t *a = &g_algorithms[pipeline_active()];
    cli_printf(o, "algorithm %u '%s':\n", a->id, a->name);
    for (uint8_t i = 0; i < a->num_stages; i++) {
        uint32_t avg, max;
        pipeline_stage_timing(i, &avg, &max);
        cli_printf(o, "  [%u] stage id %2u %-24s avg %4lu us  max %4lu us\n", i, a->stages[i]->id, a->stages[i]->name,
                   (unsigned long)hw_cycles_to_us(avg), (unsigned long)hw_cycles_to_us(max));
    }
    return 0;
}

static int c_algo(int argc, char **argv, cli_out_t *o)
{
    if (argc > 1) {
        int r = settings_set_run_config(0xFF, (uint8_t)atoi(argv[1]), 0xFF, 0xFF);
        if (r != CALIB_OK) return r;
    }
    for (uint8_t i = 0; i < NUM_ALGORITHMS; i++) {
        cli_printf(o, "%c %u %s\n", i == pipeline_active() ? '*' : ' ', i, g_algorithms[i].name);
    }
    return 0;
}

static int c_runmode(int argc, char **argv, cli_out_t *o)
{
    if (argc > 1) {
        uint8_t m = (strcmp(argv[1], "pattern") == 0 || strcmp(argv[1], "1") == 0) ? RUN_MODE_PATTERN : RUN_MODE_AUDIO;
        settings_set_run_config(m, 0xFF, 0xFF, 0xFF);
    }
    cli_printf(o, "run mode: %s\n", mode_run_mode() == RUN_MODE_AUDIO ? "audio" : "pattern");
    return 0;
}

static int c_pattern(int argc, char **argv, cli_out_t *o)
{
    if (argc > 1) {
        int r = settings_set_run_config(0xFF, 0xFF, (uint8_t)atoi(argv[1]), 0xFF);
        if (r != CALIB_OK) return r;
    }
    for (uint8_t i = 0; i < led_pattern_count(); i++) {
        cli_printf(o, "%c %u %s\n", i == mode_pattern() ? '*' : ' ', i, led_pattern_name(i));
    }
    return 0;
}

static int c_brightness(int argc, char **argv, cli_out_t *o)
{
    if (argc > 1) {
        uint8_t b = (uint8_t)atoi(argv[1]);
        bool rb;
        settings_set(SET_BRIGHTNESS, &b, 1, &rb);
    }
    cli_printf(o, "brightness %u\n", light_brightness());
    return 0;
}

static int c_mode(int argc, char **argv, cli_out_t *o)
{
    if (argc > 1) {
        dev_mode_t m = (strncmp(argv[1], "cal", 3) == 0 || strcmp(argv[1], "1") == 0) ? DEV_MODE_CALIBRATION : DEV_MODE_MAIN;
        if (mode_set(m) != HW_OK) return -2;
    }
    cli_printf(o, "device mode: %s\n", mode_get() == DEV_MODE_CALIBRATION ? "CALIBRATION" :
               (mode_get() == DEV_MODE_FW_UPDATE ? "FW_UPDATE" : "MAIN"));
    return 0;
}

static int c_params(int argc, char **argv, cli_out_t *o)
{
    if (argc < 2) {
        uint8_t ids[STAGE_COUNT];
        uint8_t n = pipeline_stage_ids(ids, STAGE_COUNT);
        for (uint8_t i = 0; i < n; i++) cli_printf(o, "stage %2u %s\n", ids[i], pipeline_stage(ids[i])->name);
        return 0;
    }
    const stage_def_t *s = pipeline_stage((uint8_t)atoi(argv[1]));
    if (!s) return -2;
    const float *v = calib_stage_values(s->id);
    cli_printf(o, "stage %u %s%s\n", s->id, s->name, calib_dirty() ? " (unsaved changes)" : "");
    for (uint8_t p = 0; p < s->num_params; p++) {
        const param_desc_t *d = &s->params[p];
        cli_printf(o, "  %2u %-18s = %10.3f %-6s [%g .. %g] def %g\n", p, d->name, (double)v[p],
                   d->type == PARAM_ENUM ? "" : d->unit, (double)d->min, (double)d->max, (double)d->def);
        if (d->type == PARAM_ENUM) cli_printf(o, "     options: %s\n", d->unit);
    }
    return 0;
}

static int c_param(int argc, char **argv, cli_out_t *o)
{
    if (argc < 3) return -1;
    uint8_t st = (uint8_t)atoi(argv[1]), p = (uint8_t)atoi(argv[2]);
    float v;
    if (argc > 3) {
        float applied;
        int r = calib_set_param(st, p, strtof(argv[3], NULL), &applied);
        if (r == CALIB_ERR_MODE) { cli_printf(o, "enter calibration mode first: mode calib\n"); return 0; }
        if (r != CALIB_OK) return r;
    }
    if (calib_get_param(st, p, &v) != CALIB_OK) return -2;
    cli_printf(o, "stage %u param %u = %g\n", st, p, (double)v);
    return 0;
}

static int c_save(int argc, char **argv, cli_out_t *o)
{
    int r = calib_save();
    cli_printf(o, "%s\n", r == CALIB_OK ? "saved" : (r == CALIB_ERR_MODE ? "enter calibration mode first" : "flash error"));
    return 0;
}

static int c_features(int argc, char **argv, cli_out_t *o)
{
    live_snapshot_t l;
    mode_get_live(&l);
    cli_printf(o, "level %5u  bpm %u.%u  beats %u  centroid %u\nbands:", l.level, l.bpm_x10 / 10, l.bpm_x10 % 10,
               l.beat_count, l.centroid);
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) cli_printf(o, " %u", l.bands[b] >> 7);
    cli_printf(o, "  (0..255)\n");
    return 0;
}

static int c_errors(int argc, char **argv, cli_out_t *o)
{
    static const char *sev[] = {"info", "warn", "ERROR", "FATAL"};
    err_entry_t e[ERR_COUNT];
    uint8_t n = err_get_entries(e, ERR_COUNT);
    if (n == 0) cli_printf(o, "no errors\n");
    for (uint8_t i = 0; i < n; i++) {
        cli_printf(o, "%2u %-16s %-5s x%u last %lu ms\n", e[i].code, e[i].name, sev[e[i].severity], e[i].count,
                   (unsigned long)e[i].last_ms);
    }
    cli_printf(o, "active: %s; last watchdog culprit: '%s'\n", err_name(err_active()), wdt_last_culprit());
    return 0;
}

static int c_clear_errors(int argc, char **argv, cli_out_t *o)
{
    err_clear_all();
    cli_printf(o, "cleared\n");
    return 0;
}

static int c_raise_error(int argc, char **argv, cli_out_t *o)
{
    if (argc < 2) return -1;
    int code = atoi(argv[1]);
    if (code <= 0 || code >= ERR_COUNT) return -2;
    err_report((err_code_t)code);
    cli_printf(o, "raised %d (%s)\n", code, err_name((err_code_t)code));
    return 0;
}

static int c_mic(int argc, char **argv, cli_out_t *o)
{
    daq_stats_t s;
    daq_get_stats(&s);
    cli_printf(o, "rate %lu Hz, channel %s%s, signal %s\nblocks %lu dropped %lu timeouts %lu, last block age %lu us\n",
               (unsigned long)s.actual_rate, s.channel == MIC_CHANNEL_LEFT ? "left" : "right",
               s.auto_detecting ? " (detecting)" : "", s.signal_ok ? "ok" : "NONE", (unsigned long)s.blocks,
               (unsigned long)s.dropped, (unsigned long)s.timeouts, (unsigned long)s.last_age_us);
    return 0;
}

static int c_latency(int argc, char **argv, cli_out_t *o)
{
    rt_stats_t r;
    mode_get_rt_stats(&r);
    uint32_t hop_us = AUDIO_HOP_SIZE * 1000000u / SAMPLE_RATE;
    uint32_t wire = ws2812_frame_time_us(LED_MAX_COUNT);
    cli_printf(o, "processing avg %lu max %lu us, block->LED avg %lu max %lu us, overruns %lu\n",
               (unsigned long)r.proc_avg_us, (unsigned long)r.proc_max_us, (unsigned long)r.latency_avg_us,
               (unsigned long)r.latency_max_us, (unsigned long)r.overruns);
    cli_printf(o, "estimated mic->light: %lu us (hop/2 %lu + processing + LED wire %lu)\n",
               (unsigned long)(hop_us / 2 + r.latency_avg_us + wire), (unsigned long)(hop_us / 2), (unsigned long)wire);
    return 0;
}

static int c_bench_dsp(int argc, char **argv, cli_out_t *o)
{
    static cq15_t buf[AUDIO_FFT_SIZE];
    static q31_t blk[AUDIO_HOP_SIZE];
    for (uint32_t i = 0; i < AUDIO_FFT_SIZE; i++) { buf[i].re = (int16_t)(i * 97); buf[i].im = 0; }
    uint32_t t0 = hw_cycles();
    dsp_fft_q15(buf, AUDIO_FFT_LOG2);
    uint32_t t_fft = hw_cycles() - t0;
    dsp_biquad_t bq = {0};
    dsp_biquad_design(&bq.c, DSP_BQ_LOWPASS, SAMPLE_RATE, 150.0f, 0.707f, 0.0f);
    for (uint32_t i = 0; i < AUDIO_HOP_SIZE; i++) blk[i] = (q31_t)(i * 0x01000000);
    t0 = hw_cycles();
    dsp_biquad_process(&bq, blk, AUDIO_HOP_SIZE);
    uint32_t t_bq = hw_cycles() - t0;
    cli_printf(o, "FFT %u-pt Q15: %lu cycles (%lu us)\nbiquad %u samples: %lu cycles (%lu us)\n", AUDIO_FFT_SIZE,
               (unsigned long)t_fft, (unsigned long)hw_cycles_to_us(t_fft), AUDIO_HOP_SIZE, (unsigned long)t_bq,
               (unsigned long)hw_cycles_to_us(t_bq));
    return 0;
}

#ifdef ENABLE_SELF_DIAGNOSTICS
static int c_diag(int argc, char **argv, cli_out_t *o)
{
    o->len += diag_report(o->buf + o->len, o->cap - o->len);
    return 0;
}
#endif

static int c_heap(int argc, char **argv, cli_out_t *o)
{
    cli_printf(o, "free %lu B, min free %lu B, largest block %u B\n", (unsigned long)esp_get_free_heap_size(),
               (unsigned long)esp_get_minimum_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return 0;
}

static int c_reboot(int argc, char **argv, cli_out_t *o)
{
    cli_printf(o, "rebooting... (use the REBOOT command for a clean response)\n");
    esp_restart();
    return 0;
}

static const cli_cmd_t s_cmds[] = {
    {"version",                 "",                         "firmware version",                       c_version},
    {"get_fw_version",          "",                         "firmware version",                       c_version},
    {"info",                    "",                         "system overview",                        c_info},
    {"set_led_strip",           "<rrggbb> <start> <end> [hold_ms]", "test colour on LEDs (overlay)",  c_set_led_strip},
    {"led_release",             "",                         "remove the set_led_strip overlay",       c_led_release},
    {"print_mic_samples",       "[n<=256]",                 "raw microphone samples",                 c_print_mic},
    {"print_processed_samples", "<stage_no> [n<=256]",      "samples after stage index (see stages)", c_print_processed},
    {"stages",                  "",                         "stages of the active algorithm + timing", c_stages},
    {"features",                "",                         "level, bpm, beats, bands",               c_features},
    {"algo",                    "[n]",                      "list / select algorithm",                c_algo},
    {"runmode",                 "[audio|pattern]",          "show / set run mode",                    c_runmode},
    {"pattern",                 "[n]",                      "list / select pattern",                  c_pattern},
    {"brightness",              "[0..255]",                 "show / set brightness",                  c_brightness},
    {"mode",                    "[main|calib]",             "show / set device mode",                 c_mode},
    {"params",                  "[stage]",                  "list stages / parameters of a stage",    c_params},
    {"param",                   "<stage> <param> [value]",  "get / set a parameter (calib mode)",     c_param},
    {"save",                    "",                         "save parameters to flash (calib mode)",  c_save},
    {"mic",                     "",                         "microphone statistics",                  c_mic},
    {"latency",                 "",                         "latency measurements",                   c_latency},
    {"bench_dsp",               "",                         "benchmark FFT and biquad",               c_bench_dsp},
    {"errors",                  "",                         "error log",                              c_errors},
    {"clear_errors",            "",                         "clear error log",                        c_clear_errors},
    {"raise_error",             "<code>",                   "inject an error (test indication)",      c_raise_error},
#ifdef ENABLE_SELF_DIAGNOSTICS
    {"diag",                    "",                         "self diagnostics report",                c_diag},
#endif
    {"heap",                    "",                         "heap usage",                             c_heap},
    {"reboot",                  "",                         "restart immediately",                    c_reboot},
};

void cli_commands_register(void)
{
    cli_register(s_cmds, sizeof(s_cmds) / sizeof(s_cmds[0]));
}

#else

void cli_commands_register(void)
{
}

#endif /* ENABLE_DEBUG_CLI */
