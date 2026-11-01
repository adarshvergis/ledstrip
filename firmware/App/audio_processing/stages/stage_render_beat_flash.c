/**
 * @file stage_render_beat_flash.c
 * @brief Render: all LEDs switch on with every beat (on/off for a fixed time, or a
 *        flash with exponential decay); hue optionally steps on each beat.
 */
#include "audio_types.h"
#include "led_color.h"

#include <math.h>

typedef struct {
    bool     flash;
    uint16_t on_blocks;
    q31_t    decay;         /* per-block multiplier */
    uint16_t hue_step;
    uint8_t  sat;
    bool     use_strength;
    uint8_t  floor;
    uint16_t base_hue;
    /* runtime */
    uint32_t hue_acc;
    q31_t    env;           /* Q31 brightness */
    uint8_t  amp;
    uint16_t on_cnt;
} rbeat_state_t;

static rbeat_state_t s_st;

static const param_desc_t s_params[RBEAT_P_COUNT] = {
    [RBEAT_P_MODE]         = PDESC_E("mode", "on/off|flash", 2, 0.0f),
    [RBEAT_P_ON_TIME_MS]   = PDESC_I("on_time", "ms", 10.0f, 1000.0f, 110.0f),
    [RBEAT_P_DECAY_MS]     = PDESC_I("decay", "ms", 20.0f, 2000.0f, 220.0f),
    [RBEAT_P_HUE]          = PDESC_I("hue", "deg", 0.0f, 359.0f, 280.0f),
    [RBEAT_P_SATURATION]   = PDESC_F("saturation", "", 0.0f, 1.0f, 1.0f, 0.01f),
    [RBEAT_P_HUE_STEP]     = PDESC_I("hue_step", "deg", 0.0f, 180.0f, 40.0f),
    [RBEAT_P_USE_STRENGTH] = PDESC_B("use_strength", 1.0f),
    [RBEAT_P_FLOOR]        = PDESC_F("floor", "", 0.0f, 0.5f, 0.0f, 0.01f),
};

static void reset(void *st)
{
    rbeat_state_t *s = st;
    s->env = 0;
    s->on_cnt = 0;
    s->hue_acc = 0;
}

static void configure(void *st, const float *p)
{
    rbeat_state_t *s = st;
    s->flash = p[RBEAT_P_MODE] > 0.5f;
    s->on_blocks = (uint16_t)(p[RBEAT_P_ON_TIME_MS] / BLOCK_MS + 0.5f);
    s->decay = dsp_float_to_q31(expf(-BLOCK_MS / p[RBEAT_P_DECAY_MS]));
    s->base_hue = (uint16_t)p[RBEAT_P_HUE];
    s->sat = (uint8_t)(p[RBEAT_P_SATURATION] * 255.0f);
    s->hue_step = (uint16_t)p[RBEAT_P_HUE_STEP];
    s->use_strength = p[RBEAT_P_USE_STRENGTH] > 0.5f;
    s->floor = (uint8_t)(p[RBEAT_P_FLOOR] * 255.0f);
}

static void process(void *st, audio_frame_t *f)
{
    rbeat_state_t *s = st;
    if (f->beat) {
        uint32_t a = 255;
        if (s->use_strength) {
            a = 90u + ((uint32_t)f->beat_strength * 165u >> 15);
        }
        s->amp = (uint8_t)a;
        s->env = (q31_t)(((int64_t)a << 31) / 256);
        s->on_cnt = s->on_blocks;
        s->hue_acc += s->hue_step;
    }

    uint8_t bri;
    if (s->flash) {
        bri = (uint8_t)(s->env >> 23);
        s->env = dsp_mul_q31(s->env, s->decay);
        bri = led_gamma8(bri);
    } else {
        bri = s->on_cnt ? s->amp : 0;
        if (s->on_cnt) s->on_cnt--;
    }
    if (bri < s->floor) bri = s->floor;
    uint16_t hue = (uint16_t)((s->base_hue + s->hue_acc) % 360u);
    led_fill(f->leds, f->num_leds, led_hsv(hue, s->sat, bri));
}

const stage_def_t g_stage_render_beat_flash = {
    .id = STAGE_RENDER_BEAT_FLASH,
    .name = "Render: beat (all)",
    .desc = "All LEDs on with every detected beat",
    .params = s_params,
    .num_params = RBEAT_P_COUNT,
    .state = &s_st,
    .reset = reset,
    .configure = configure,
    .process = process,
};
