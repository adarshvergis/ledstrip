/**
 * @file pipeline.c
 * @brief Pipeline execution, deferred configuration, capture taps and timing.
 */
#include "pipeline.h"

#include <string.h>
#include "hw_common.h"
#include "calibration.h"
#include "spectrum_taps.h"

#include "freertos/FreeRTOS.h"
#include "esp_log.h"

static const char *TAG = "pipeline";

/* Every stage the firmware knows, indexed by stage id (NULL = retired id). */
static const stage_def_t *const s_all_stages[STAGE_COUNT] = {
    [STAGE_FRONTEND]             = &g_stage_frontend,
    [STAGE_FILTER_A]             = &g_stage_filter_a,
    [STAGE_FILTER_B]             = &g_stage_filter_b,
    [STAGE_ENVELOPE]             = &g_stage_envelope,
    [STAGE_AGC]                  = &g_stage_agc,
    [STAGE_SPECTRUM]             = &g_stage_spectrum,
    [STAGE_BEAT]                 = &g_stage_beat,
    [STAGE_RENDER_BEAT_FLASH]    = &g_stage_render_beat_flash,
    [STAGE_RENDER_SPECTRUM]      = &g_stage_render_spectrum,
    [STAGE_RENDER_BEAT_SEGMENTS] = &g_stage_render_beat_segments,
    [STAGE_MELODY]               = &g_stage_melody,
    [STAGE_MUSIC]                = &g_stage_music,
    [STAGE_RENDER_SHOW]          = &g_stage_render_show,
};

_Static_assert(STAGE_COUNT <= 32, "dirty mask is 32 bit");

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t  s_active;
static volatile int16_t  s_requested = -1;
static volatile uint32_t s_dirty_mask;

/* capture tap */
static q31_t    s_cap_buf[PIPELINE_CAPTURE_MAX];
static volatile int8_t   s_cap_stage;
static volatile uint16_t s_cap_want;
static volatile uint16_t s_cap_have;
static volatile bool     s_cap_active;

/* timing per stage position */
static uint32_t s_t_max[PIPELINE_MAX_STAGES];
static uint64_t s_t_sum[PIPELINE_MAX_STAGES];
static uint32_t s_t_cnt;

const stage_def_t *pipeline_stage(uint8_t stage_id)
{
    return (stage_id < STAGE_COUNT) ? s_all_stages[stage_id] : NULL;
}

uint8_t pipeline_stage_ids(uint8_t *out, uint8_t max)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < STAGE_COUNT && n < max; i++) {
        if (s_all_stages[i]) out[n++] = i;
    }
    return n;
}

static void configure_stage(const stage_def_t *s)
{
    if (s->configure) s->configure(s->state, calib_stage_values(s->id));
}

static void start_algorithm(uint8_t algo)
{
    const algorithm_def_t *a = &g_algorithms[algo];
    for (uint8_t i = 0; i < a->num_stages; i++) {
        const stage_def_t *s = a->stages[i];
        configure_stage(s);
        if (s->reset) s->reset(s->state);
    }
    s_active = algo;
    pipeline_reset_timing();
    ESP_LOGI(TAG, "algorithm %u '%s' (%u stages)", algo, a->name, a->num_stages);
}

void pipeline_init(uint8_t algorithm)
{
    for (uint8_t i = 0; i < NUM_ALGORITHMS; i++) {
        configASSERT(g_algorithms[i].id == i && g_algorithms[i].num_stages <= PIPELINE_MAX_STAGES);
    }
    s_dirty_mask = 0;
    start_algorithm(algorithm < NUM_ALGORITHMS ? algorithm : DEFAULT_ALGORITHM);
}

int pipeline_select(uint8_t algorithm)
{
    if (algorithm >= NUM_ALGORITHMS) return HW_ERR_ARG;
    s_requested = algorithm;
    return HW_OK;
}

uint8_t pipeline_active(void)
{
    int16_t r = s_requested;
    return (r >= 0) ? (uint8_t)r : s_active;
}

void pipeline_params_changed(uint8_t stage)
{
    if (stage >= STAGE_COUNT) return;
    portENTER_CRITICAL(&s_lock);
    s_dirty_mask |= (1u << stage);
    portEXIT_CRITICAL(&s_lock);
}

static void capture(int8_t pos, const audio_frame_t *f)
{
    if (!s_cap_active || s_cap_stage != pos) return;
    uint16_t have = s_cap_have;
    uint16_t take = f->n;
    if (have + take > s_cap_want) take = s_cap_want - have;
    memcpy(&s_cap_buf[have], f->samples, take * sizeof(q31_t));
    s_cap_have = have + take;
    if (s_cap_have >= s_cap_want) s_cap_active = false;
}

void pipeline_process(audio_frame_t *f)
{
    /* apply pending requests */
    int16_t req = s_requested;
    if (req >= 0) {
        s_requested = -1;
        start_algorithm((uint8_t)req);
    }
    uint32_t dirty;
    portENTER_CRITICAL(&s_lock);
    dirty = s_dirty_mask;
    s_dirty_mask = 0;
    portEXIT_CRITICAL(&s_lock);
    if (dirty) {
        for (uint8_t id = 0; id < STAGE_COUNT; id++) {
            if ((dirty & (1u << id)) && s_all_stages[id]) configure_stage(s_all_stages[id]);
        }
    }

    const algorithm_def_t *a = &g_algorithms[s_active];
    const uint16_t taps = taps_mask();      /* live spectrum taps chosen by the host */
    capture(PIPELINE_CAPTURE_INPUT, f);
    for (uint8_t i = 0; i < a->num_stages; i++) {
        const stage_def_t *s = a->stages[i];
        uint32_t t0 = hw_cycles();
        s->process(s->state, f);
        uint32_t dt = hw_cycles() - t0;
        s_t_sum[i] += dt;
        if (dt > s_t_max[i]) s_t_max[i] = dt;
        capture((int8_t)i, f);
        if (taps & (1u << s->id)) taps_capture(s->id, f->samples, f->n);
    }
    if (taps & (1u << TAP_END)) taps_capture(TAP_END, f->samples, f->n);
    s_t_cnt++;
}

int pipeline_capture_start(int8_t stage_index, uint16_t n)
{
    if (s_cap_active) return HW_ERR_BUSY;
    if (n == 0 || n > PIPELINE_CAPTURE_MAX) return HW_ERR_ARG;
    if (stage_index < PIPELINE_CAPTURE_INPUT || stage_index >= (int8_t)g_algorithms[s_active].num_stages) {
        return HW_ERR_ARG;
    }
    s_cap_stage = stage_index;
    s_cap_want = n;
    s_cap_have = 0;
    s_cap_active = true;
    return HW_OK;
}

bool pipeline_capture_done(void)
{
    return !s_cap_active && s_cap_have > 0 && s_cap_have >= s_cap_want;
}

const q31_t *pipeline_capture_data(uint16_t *n)
{
    if (n) *n = s_cap_have;
    return s_cap_buf;
}

void pipeline_stage_timing(uint8_t idx, uint32_t *avg, uint32_t *max)
{
    if (idx >= PIPELINE_MAX_STAGES) {
        *avg = *max = 0;
        return;
    }
    *avg = s_t_cnt ? (uint32_t)(s_t_sum[idx] / s_t_cnt) : 0;
    *max = s_t_max[idx];
}

void pipeline_reset_timing(void)
{
    memset(s_t_max, 0, sizeof(s_t_max));
    memset(s_t_sum, 0, sizeof(s_t_sum));
    s_t_cnt = 0;
}
