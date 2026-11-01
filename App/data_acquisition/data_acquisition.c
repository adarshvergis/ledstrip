/**
 * @file data_acquisition.c
 * @brief DMA block hand-off, channel auto-detection, signal supervision.
 */
#include "data_acquisition.h"

#include <string.h>
#include "build_config.h"
#include "hw_common.h"
#include "inmp441.h"
#include "error_handler.h"

#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "daq";

#define AUTODETECT_BLOCKS   64u
#define NO_SIGNAL_P2P       16384u      /* Q31: practically constant data */

static TaskHandle_t       s_consumer;
static volatile uint32_t  s_latest_idx;
static volatile uint32_t  s_latest_cycles;
static uint32_t           s_read_cycles;
static uint8_t            s_req_channel = MIC_CHANNEL_DEFAULT;
static uint8_t            s_channel = MIC_CHANNEL_LEFT;
static bool               s_detecting;
static uint32_t           s_detect_n;
static uint64_t           s_detect_sum[2];
static uint32_t           s_flat_blocks;
static bool               s_signal_ok = true;
static daq_stats_t        s_stats;

static void IRAM_ATTR on_block(uint32_t idx, void *arg)
{
    s_latest_idx = idx;
    s_latest_cycles = hw_cycles();
    BaseType_t woken = pdFALSE;
    if (s_consumer) {
        vTaskNotifyGiveFromISR(s_consumer, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

int daq_init(TaskHandle_t consumer)
{
    s_consumer = consumer;
    mic_config_t cfg = {
        .pin_sck = PIN_MIC_SCK,
        .pin_ws = PIN_MIC_WS,
        .pin_sd = PIN_MIC_SD,
        .sample_rate = SAMPLE_RATE,
        .frames_per_block = AUDIO_HOP_SIZE,
        .num_blocks = AUDIO_DMA_BLOCKS,
    };
    int r = mic_init(&cfg, on_block, NULL);
    if (r != HW_OK) {
        err_report(ERR_MIC_INIT);
        return r;
    }
    ESP_LOGI(TAG, "mic ready: %lu Hz (requested %u), %u-sample blocks",
             (unsigned long)mic_actual_sample_rate(), (unsigned)SAMPLE_RATE, (unsigned)AUDIO_HOP_SIZE);
    daq_set_channel(s_req_channel);
    return HW_OK;
}

int daq_start(void)
{
    ulTaskNotifyTake(pdTRUE, 0);    /* discard stale notifications */
    s_flat_blocks = 0;
    if (s_req_channel == MIC_CHANNEL_AUTO) {
        s_detecting = true;
        s_detect_n = 0;
        s_detect_sum[0] = s_detect_sum[1] = 0;
    }
    return mic_start();
}

void daq_stop(void)
{
    mic_stop();
}

bool daq_running(void)
{
    return mic_is_running();
}

void daq_set_channel(uint8_t ch)
{
    s_req_channel = ch;
    if (ch == MIC_CHANNEL_AUTO) {
        s_detecting = true;
        s_detect_n = 0;
        s_detect_sum[0] = s_detect_sum[1] = 0;
    } else {
        s_detecting = false;
        s_channel = (ch == MIC_CHANNEL_RIGHT) ? MIC_CHANNEL_RIGHT : MIC_CHANNEL_LEFT;
    }
}

static void supervise(const int32_t *blk)
{
    if (s_detecting) {
        for (uint32_t i = 0; i < AUDIO_HOP_SIZE; i++) {
            s_detect_sum[0] += dsp_abs_q31(blk[2 * i]) >> 8;
            s_detect_sum[1] += dsp_abs_q31(blk[2 * i + 1]) >> 8;
        }
        if (++s_detect_n >= AUTODETECT_BLOCKS) {
            s_detecting = false;
            s_channel = (s_detect_sum[1] > s_detect_sum[0]) ? MIC_CHANNEL_RIGHT : MIC_CHANNEL_LEFT;
            ESP_LOGI(TAG, "auto channel: %s (L=%llu R=%llu)", s_channel == MIC_CHANNEL_LEFT ? "left" : "right",
                     (unsigned long long)s_detect_sum[0], (unsigned long long)s_detect_sum[1]);
        }
    }

    int32_t mn = INT32_MAX, mx = INT32_MIN;
    for (uint32_t i = 0; i < AUDIO_HOP_SIZE; i++) {
        int32_t v = blk[2 * i + s_channel];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    if ((uint32_t)((int64_t)mx - mn) < NO_SIGNAL_P2P) {
        if (++s_flat_blocks == MIC_NO_SIGNAL_BLOCKS) {
            s_signal_ok = false;
            err_report(ERR_MIC_NO_SIGNAL);
        }
    } else {
        if (!s_signal_ok) {
            s_signal_ok = true;
            err_clear(ERR_MIC_NO_SIGNAL);
        }
        s_flat_blocks = 0;
    }
}

int daq_read_block(q31_t *out, uint32_t timeout_ms)
{
    uint32_t n = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if (n == 0) {
        s_stats.timeouts++;
        return HW_ERR_TIMEOUT;
    }
    if (n > 1) {
        s_stats.dropped += n - 1;   /* process only the newest block: minimum latency */
    }
    uint32_t idx = s_latest_idx;
    s_read_cycles = s_latest_cycles;
    const int32_t *blk = mic_block(idx);

    supervise(blk);
    const uint32_t ch = s_channel;
    for (uint32_t i = 0; i < AUDIO_HOP_SIZE; i++) {
        out[i] = blk[2 * i + ch];   /* 24-bit left aligned == Q31 */
    }
    s_stats.blocks++;
    s_stats.last_age_us = hw_cycles_to_us(hw_cycles() - s_read_cycles);
    return HW_OK;
}

uint32_t daq_last_block_cycles(void)
{
    return s_read_cycles;
}

void daq_get_stats(daq_stats_t *st)
{
    *st = s_stats;
    st->channel = s_channel;
    st->auto_detecting = s_detecting;
    st->signal_ok = s_signal_ok;
    st->actual_rate = mic_actual_sample_rate();
}
