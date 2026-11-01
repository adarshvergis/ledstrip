/**
 * @file stage_render_beat_segments.c
 * @brief Render: every beat spawns a light pulse. Which LEDs = position derived from
 *        the spectral centroid (bright/treble-heavy sound -> right, bassy -> left) or
 *        the dominant band; how many LEDs = beat strength. Pulses fade and spread.
 */
#include "audio_types.h"
#include "led_color.h"

#include <math.h>
#include <string.h>

#define MAX_PULSES  8

typedef struct {
    int32_t  center_q8;     /* LED position, Q8 */
    int32_t  half_q8;       /* half width, Q8 */
    q31_t    amp;
    uint16_t hue;
} pulse_t;

typedef struct {
    int32_t  min_w, max_w;  /* LEDs */
    q31_t    decay;
    uint8_t  pos_mode;
    uint8_t  hue_mode;
    uint16_t hue;
    uint8_t  sat;
    uint8_t  max_pulses;
    bool     spread;
    /* runtime */
    pulse_t  p[MAX_PULSES];
    uint8_t  next;
    int32_t  cmin, cmax;    /* adaptive centroid range, Q15 */
    uint32_t sweep_q16;
    uint16_t hue_cycle;
} rseg_state_t;

static rseg_state_t s_st;

static const param_desc_t s_params[RSEG_P_COUNT] = {
    [RSEG_P_MIN_WIDTH]  = PDESC_I("min_width", "LEDs", 1.0f, 30.0f, 3.0f),
    [RSEG_P_MAX_WIDTH]  = PDESC_I("max_width", "LEDs", 2.0f, 60.0f, 18.0f),
    [RSEG_P_DECAY_MS]   = PDESC_I("decay", "ms", 50.0f, 3000.0f, 450.0f),
    [RSEG_P_POSITION]   = PDESC_E("position", "centroid|dominant band|sweep", 3, 0.0f),
    [RSEG_P_HUE_MODE]   = PDESC_E("hue_mode", "position|beat cycle|fixed", 3, 0.0f),
    [RSEG_P_HUE]        = PDESC_I("hue", "deg", 0.0f, 359.0f, 200.0f),
    [RSEG_P_SATURATION] = PDESC_F("saturation", "", 0.0f, 1.0f, 1.0f, 0.01f),
    [RSEG_P_MAX_PULSES] = PDESC_I("max_pulses", "", 1.0f, (float)MAX_PULSES, 5.0f),
    [RSEG_P_SPREAD]     = PDESC_B("spread", 1.0f),
};

static void reset(void *st)
{
    rseg_state_t *s = st;
    memset(s->p, 0, sizeof(s->p));
    s->next = 0;
    s->cmin = 1500;         /* ~0.05 Nyquist */
    s->cmax = 9000;         /* ~0.27 Nyquist */
    s->sweep_q16 = 0;
    s->hue_cycle = 0;
}

static void configure(void *st, const float *p)
{
    rseg_state_t *s = st;
    s->min_w = (int32_t)p[RSEG_P_MIN_WIDTH];
    s->max_w = (int32_t)p[RSEG_P_MAX_WIDTH];
    if (s->max_w < s->min_w) s->max_w = s->min_w;
    s->decay = dsp_float_to_q31(expf(-BLOCK_MS / p[RSEG_P_DECAY_MS]));
    s->pos_mode = (uint8_t)(p[RSEG_P_POSITION] + 0.5f);
    s->hue_mode = (uint8_t)(p[RSEG_P_HUE_MODE] + 0.5f);
    s->hue = (uint16_t)p[RSEG_P_HUE];
    s->sat = (uint8_t)(p[RSEG_P_SATURATION] * 255.0f);
    s->max_pulses = (uint8_t)p[RSEG_P_MAX_PULSES];
    if (s->max_pulses < 1) s->max_pulses = 1;
    if (s->max_pulses > MAX_PULSES) s->max_pulses = MAX_PULSES;
    s->spread = p[RSEG_P_SPREAD] > 0.5f;
}

static void process(void *st, audio_frame_t *f)
{
    rseg_state_t *s = st;
    const int32_t n = f->num_leds;

    /* track the range the centroid moves in, so positions use the whole strip */
    int32_t c = f->centroid;
    if (c < s->cmin) s->cmin = c; else s->cmin += (c - s->cmin) >> 10;
    if (c > s->cmax) s->cmax = c; else s->cmax -= (s->cmax - c) >> 10;
    if (s->cmax - s->cmin < 1000) s->cmax = s->cmin + 1000;

    if (f->beat) {
        int32_t pos_q15;
        switch (s->pos_mode) {
        case 1:  pos_q15 = ((2 * f->dominant_band + 1) * 32767) / (2 * AUDIO_NUM_BANDS); break;
        case 2:  s->sweep_q16 += 40503u;   /* golden ratio step */
                 pos_q15 = (int32_t)((s->sweep_q16 & 0xFFFF) >> 1); break;
        default: pos_q15 = ((c - s->cmin) * 32767) / (s->cmax - s->cmin); break;
        }
        if (pos_q15 < 0) pos_q15 = 0;
        if (pos_q15 > 32767) pos_q15 = 32767;

        int32_t width = s->min_w + (((s->max_w - s->min_w) * (int32_t)f->beat_strength) >> 15);
        uint16_t hue;
        if (s->hue_mode == 0) hue = (uint16_t)((pos_q15 * 300) >> 15);
        else if (s->hue_mode == 1) hue = (s->hue_cycle = (uint16_t)((s->hue_cycle + 47) % 360));
        else hue = s->hue;

        pulse_t *pl = &s->p[s->next];
        s->next = (uint8_t)((s->next + 1) % s->max_pulses);
        pl->center_q8 = ((n - 1) * pos_q15) >> 7;      /* (n-1) * pos * 256 / 32768 */
        pl->half_q8 = width * 128;
        pl->amp = Q31_ONE;
        pl->hue = hue;
    }

    led_fill(f->leds, (uint16_t)n, LED_BLACK);
    for (int k = 0; k < s->max_pulses; k++) {
        pulse_t *pl = &s->p[k];
        if (pl->amp < 0x00800000) {     /* < 1/256: dead */
            pl->amp = 0;
            continue;
        }
        uint8_t a = (uint8_t)(pl->amp >> 23);
        int32_t hw = pl->half_q8 > 128 ? pl->half_q8 : 128;
        int32_t lo = (pl->center_q8 - hw) >> 8, hi = (pl->center_q8 + hw) >> 8;
        if (lo < 0) lo = 0;
        if (hi > n - 1) hi = n - 1;
        for (int32_t i = lo; i <= hi; i++) {
            int32_t d = i * 256 - pl->center_q8;
            if (d < 0) d = -d;
            if (d >= hw) continue;
            uint8_t v = (uint8_t)(((int32_t)a * (hw - d)) / hw);
            f->leds[i] = led_add(f->leds[i], led_hsv(pl->hue, s->sat, led_gamma8(v)));
        }
        pl->amp = dsp_mul_q31(pl->amp, s->decay);
        if (s->spread && pl->half_q8 < s->max_w * 256) pl->half_q8 += pl->half_q8 >> 6;
    }
}

const stage_def_t g_stage_render_beat_segments = {
    .id = STAGE_RENDER_BEAT_SEGMENTS,
    .name = "Render: beat segments",
    .desc = "Beat pulses; position = spectral centroid, width = beat strength",
    .params = s_params,
    .num_params = RSEG_P_COUNT,
    .state = &s_st,
    .reset = reset,
    .configure = configure,
    .process = process,
};
