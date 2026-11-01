/**
 * @file stage_envelope.c
 * @brief Amplitude envelope follower (per block RMS or peak, attack/release smoothing).
 */
#include "audio_types.h"

/* 20*log10(2^31) in Q8: converts Q31 amplitude to dBFS */
#define Q31_FS_DB_Q8    47780

typedef struct {
    dsp_env_t env;
    bool peak;
} envelope_state_t;

static envelope_state_t s_st;

static const param_desc_t s_params[ENVELOPE_P_COUNT] = {
    [ENVELOPE_P_ATTACK_MS]  = PDESC_F("attack", "ms", 0.5f, 200.0f, 4.0f, 0.5f),
    [ENVELOPE_P_RELEASE_MS] = PDESC_F("release", "ms", 5.0f, 2000.0f, 150.0f, 5.0f),
    [ENVELOPE_P_DETECTOR]   = PDESC_E("detector", "rms|peak", 2, 0.0f),
};

static void reset(void *st)
{
    ((envelope_state_t *)st)->env.y = 0;
}

static void configure(void *st, const float *p)
{
    envelope_state_t *s = st;
    dsp_env_config(&s->env, p[ENVELOPE_P_ATTACK_MS], p[ENVELOPE_P_RELEASE_MS], BLOCK_RATE_HZ);
    s->peak = (p[ENVELOPE_P_DETECTOR] > 0.5f);
}

static void process(void *st, audio_frame_t *f)
{
    envelope_state_t *s = st;
    uint32_t x = s->peak ? dsp_peak_abs(f->samples, f->n) : dsp_rms(f->samples, f->n);
    if (x > INT32_MAX) x = INT32_MAX;
    int32_t y = dsp_env_update(&s->env, (int32_t)x);
    f->envelope = (uint32_t)y;
    f->envelope_db = dsp_amp_to_db_q8((uint64_t)(y > 0 ? y : 1)) - Q31_FS_DB_Q8;
}

const stage_def_t g_stage_envelope = {
    .id = STAGE_ENVELOPE,
    .name = "Envelope",
    .desc = "Amplitude envelope (RMS or peak) with attack/release",
    .params = s_params,
    .num_params = ENVELOPE_P_COUNT,
    .state = &s_st,
    .reset = reset,
    .configure = configure,
    .process = process,
};
