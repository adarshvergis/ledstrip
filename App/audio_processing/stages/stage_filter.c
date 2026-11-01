/**
 * @file stage_filter.c
 * @brief Generic biquad filter stage; instantiated twice (FILTER_A, FILTER_B) so a
 *        high-pass and an EQ/band-pass can be tuned independently.
 */
#include "audio_types.h"

typedef struct {
    dsp_biquad_t bq;
} filter_state_t;

static filter_state_t s_st_a;
static filter_state_t s_st_b;

#define FILTER_TYPES "off|lowpass|highpass|bandpass|notch|peak|lowshelf|highshelf"

static const param_desc_t s_params_a[FILTER_P_COUNT] = {
    [FILTER_P_TYPE]    = PDESC_E("type", FILTER_TYPES, 8, (float)DSP_BQ_HIGHPASS),
    [FILTER_P_FREQ_HZ] = PDESC_F("frequency", "Hz", 20.0f, 7900.0f, 45.0f, 1.0f),
    [FILTER_P_Q]       = PDESC_F("q", "", 0.1f, 10.0f, 0.707f, 0.01f),
    [FILTER_P_GAIN_DB] = PDESC_F("gain", "dB", -24.0f, 24.0f, 0.0f, 0.5f),
};

static const param_desc_t s_params_b[FILTER_P_COUNT] = {
    [FILTER_P_TYPE]    = PDESC_E("type", FILTER_TYPES, 8, (float)DSP_BQ_BYPASS),
    [FILTER_P_FREQ_HZ] = PDESC_F("frequency", "Hz", 20.0f, 7900.0f, 1000.0f, 1.0f),
    [FILTER_P_Q]       = PDESC_F("q", "", 0.1f, 10.0f, 0.707f, 0.01f),
    [FILTER_P_GAIN_DB] = PDESC_F("gain", "dB", -24.0f, 24.0f, 0.0f, 0.5f),
};

static void reset(void *st)
{
    dsp_biquad_reset(&((filter_state_t *)st)->bq);
}

static void configure(void *st, const float *p)
{
    filter_state_t *s = st;
    int type = (int)(p[FILTER_P_TYPE] + 0.5f);
    dsp_biquad_design(&s->bq.c, (dsp_biquad_type_t)type, (float)SAMPLE_RATE, p[FILTER_P_FREQ_HZ],
                      p[FILTER_P_Q], p[FILTER_P_GAIN_DB]);
}

static void process(void *st, audio_frame_t *f)
{
    dsp_biquad_process(&((filter_state_t *)st)->bq, f->samples, f->n);
}

const stage_def_t g_stage_filter_a = {
    .id = STAGE_FILTER_A,
    .name = "Filter A",
    .desc = "Biquad filter (default: 45 Hz high-pass against rumble)",
    .params = s_params_a,
    .num_params = FILTER_P_COUNT,
    .state = &s_st_a,
    .reset = reset,
    .configure = configure,
    .process = process,
};

const stage_def_t g_stage_filter_b = {
    .id = STAGE_FILTER_B,
    .name = "Filter B",
    .desc = "Biquad filter (default: off)",
    .params = s_params_b,
    .num_params = FILTER_P_COUNT,
    .state = &s_st_b,
    .reset = reset,
    .configure = configure,
    .process = process,
};
