/**
 * @file stage_agc.c
 * @brief Automatic level normalisation: maps the envelope to 0..1 relative to a
 *        decaying peak reference, with a noise gate and a maximum gain.
 *
 * ref = max(peak, -max_gain) - headroom      (all in dBFS)
 * log curve:    level = (env - (ref - range)) / range
 * linear curve: level = 10^((env - ref) / 20)
 */
#include "audio_types.h"

#define DB16(x)     ((int32_t)((x) * 65536.0f))
#define GATE_HYST   DB16(3.0f)

typedef struct {
    int32_t peak;           /* dB Q16 */
    int32_t decay;          /* dB Q16 per block */
    int32_t gate;
    int32_t min_ref;        /* -max_gain */
    int32_t headroom;
    int32_t range;
    bool    log_curve;
    bool    gated;
} agc_state_t;

static agc_state_t s_st;

static const param_desc_t s_params[AGC_P_COUNT] = {
    [AGC_P_PEAK_DECAY_DB_S] = PDESC_F("peak_decay", "dB/s", 0.2f, 30.0f, 3.0f, 0.1f),
    [AGC_P_GATE_DB]         = PDESC_F("noise_gate", "dBFS", -100.0f, -20.0f, -62.0f, 1.0f),
    [AGC_P_MAX_GAIN_DB]     = PDESC_F("max_gain", "dB", 0.0f, 70.0f, 40.0f, 1.0f),
    [AGC_P_HEADROOM_DB]     = PDESC_F("headroom", "dB", 0.0f, 12.0f, 2.0f, 0.5f),
    [AGC_P_CURVE]           = PDESC_E("curve", "log|linear", 2, 0.0f),
    [AGC_P_RANGE_DB]        = PDESC_F("range", "dB", 6.0f, 60.0f, 24.0f, 1.0f),
};

static void reset(void *st)
{
    agc_state_t *s = st;
    s->peak = DB16(-40.0f);
    s->gated = true;
}

static void configure(void *st, const float *p)
{
    agc_state_t *s = st;
    s->decay = DB16(p[AGC_P_PEAK_DECAY_DB_S] / BLOCK_RATE_HZ);
    s->gate = DB16(p[AGC_P_GATE_DB]);
    s->min_ref = DB16(-p[AGC_P_MAX_GAIN_DB]);
    s->headroom = DB16(p[AGC_P_HEADROOM_DB]);
    s->range = DB16(p[AGC_P_RANGE_DB]);
    s->log_curve = (p[AGC_P_CURVE] < 0.5f);
}

static void process(void *st, audio_frame_t *f)
{
    agc_state_t *s = st;
    int32_t e = f->envelope_db * 256;       /* Q8 -> Q16 */

    if (e > s->peak) {
        s->peak = e;
    } else {
        s->peak -= s->decay;
        if (s->peak < s->gate) s->peak = s->gate;
    }

    if (!s->gated && e < s->gate) s->gated = true;
    else if (s->gated && e > s->gate + GATE_HYST) s->gated = false;

    if (s->gated) {
        f->level = 0;
        return;
    }
    int32_t ref = (s->peak > s->min_ref ? s->peak : s->min_ref) - s->headroom;
    int32_t lvl;
    if (s->log_curve) {
        int64_t num = (int64_t)(e - (ref - s->range)) * 32767;
        lvl = (int32_t)(num / s->range);
    } else {
        uint32_t lin = dsp_db_q8_to_lin_q16((e - ref) >> 8);    /* Q16, 1.0 = 65536 */
        lvl = (int32_t)(lin > 65534u ? 32767u : lin >> 1);
    }
    if (lvl < 0) lvl = 0;
    if (lvl > 32767) lvl = 32767;
    f->level = (uint16_t)lvl;
}

const stage_def_t g_stage_agc = {
    .id = STAGE_AGC,
    .name = "AGC",
    .desc = "Normalises the envelope to 0..1 against a decaying peak, with noise gate",
    .params = s_params,
    .num_params = AGC_P_COUNT,
    .state = &s_st,
    .reset = reset,
    .configure = configure,
    .process = process,
};
