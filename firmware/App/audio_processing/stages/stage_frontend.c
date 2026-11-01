/**
 * @file stage_frontend.c
 * @brief Input conditioning: microphone channel selection, DC removal, input gain.
 */
#include "audio_types.h"
#include "data_acquisition.h"

typedef struct {
    dsp_dcblock_t dc;
    int32_t gain_q16;
} frontend_state_t;

static frontend_state_t s_st;

static const param_desc_t s_params[FRONTEND_P_COUNT] = {
    [FRONTEND_P_GAIN_DB]       = PDESC_F("input_gain", "dB", 0.0f, 48.0f, 18.0f, 0.5f),
    [FRONTEND_P_DC_CUTOFF_HZ]  = PDESC_F("dc_cutoff", "Hz", 5.0f, 200.0f, 30.0f, 1.0f),
    [FRONTEND_P_MIC_CHANNEL]   = PDESC_E("mic_channel", "auto|left|right", 3, 0.0f),
};

static void reset(void *st)
{
    frontend_state_t *s = st;
    s->dc.x1 = 0;
    s->dc.y1 = 0;
}

static void configure(void *st, const float *p)
{
    static const uint8_t ch_map[3] = {MIC_CHANNEL_AUTO, MIC_CHANNEL_LEFT, MIC_CHANNEL_RIGHT};
    frontend_state_t *s = st;
    q31_t x1 = s->dc.x1, y1 = s->dc.y1;
    dsp_dcblock_init(&s->dc, (float)SAMPLE_RATE, p[FRONTEND_P_DC_CUTOFF_HZ]);
    s->dc.x1 = x1;
    s->dc.y1 = y1;
    s->gain_q16 = dsp_gain_db_to_q16(p[FRONTEND_P_GAIN_DB]);
    int ch = (int)(p[FRONTEND_P_MIC_CHANNEL] + 0.5f);
    daq_set_channel(ch_map[(ch >= 0 && ch < 3) ? ch : 0]);
}

static void process(void *st, audio_frame_t *f)
{
    frontend_state_t *s = st;
    dsp_dcblock_process(&s->dc, f->samples, f->n);
    dsp_gain(f->samples, f->n, s->gain_q16);
}

const stage_def_t g_stage_frontend = {
    .id = STAGE_FRONTEND,
    .name = "Front-end",
    .desc = "Microphone channel, DC removal and input gain",
    .params = s_params,
    .num_params = FRONTEND_P_COUNT,
    .state = &s_st,
    .reset = reset,
    .configure = configure,
    .process = process,
};
