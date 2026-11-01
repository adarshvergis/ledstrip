/**
 * @file stage_render_level.c
 * @brief Render: all LEDs switch on/off (threshold + hysteresis + hold) or dim with
 *        the normalised level.
 */
#include "audio_types.h"
#include "led_color.h"

typedef struct {
    bool     dimming;
    uint16_t hue;
    uint8_t  sat;
    int32_t  thr;           /* Q15 */
    int32_t  hyst;          /* Q15 */
    uint16_t hold_blocks;
    uint8_t  min_bri;
    uint16_t hue_spread;
    bool     on;
    uint16_t hold;
} rlevel_state_t;

static rlevel_state_t s_st;

static const param_desc_t s_params[RLEVEL_P_COUNT] = {
    [RLEVEL_P_MODE]           = PDESC_E("mode", "on/off|dimming", 2, 0.0f),
    [RLEVEL_P_HUE]            = PDESC_I("hue", "deg", 0.0f, 359.0f, 30.0f),
    [RLEVEL_P_SATURATION]     = PDESC_F("saturation", "", 0.0f, 1.0f, 1.0f, 0.01f),
    [RLEVEL_P_THRESHOLD]      = PDESC_F("threshold", "", 0.0f, 1.0f, 0.5f, 0.01f),
    [RLEVEL_P_HYSTERESIS]     = PDESC_F("hysteresis", "", 0.0f, 0.5f, 0.12f, 0.01f),
    [RLEVEL_P_HOLD_MS]        = PDESC_I("hold", "ms", 0.0f, 1000.0f, 40.0f),
    [RLEVEL_P_MIN_BRIGHTNESS] = PDESC_F("min_brightness", "", 0.0f, 1.0f, 0.0f, 0.01f),
    [RLEVEL_P_HUE_SPREAD]     = PDESC_I("hue_spread", "deg", 0.0f, 360.0f, 0.0f),
};

static void reset(void *st)
{
    rlevel_state_t *s = st;
    s->on = false;
    s->hold = 0;
}

static void configure(void *st, const float *p)
{
    rlevel_state_t *s = st;
    s->dimming = p[RLEVEL_P_MODE] > 0.5f;
    s->hue = (uint16_t)p[RLEVEL_P_HUE];
    s->sat = (uint8_t)(p[RLEVEL_P_SATURATION] * 255.0f);
    s->thr = (int32_t)(p[RLEVEL_P_THRESHOLD] * 32767.0f);
    s->hyst = (int32_t)(p[RLEVEL_P_HYSTERESIS] * 32767.0f);
    s->hold_blocks = (uint16_t)(p[RLEVEL_P_HOLD_MS] / BLOCK_MS);
    s->min_bri = (uint8_t)(p[RLEVEL_P_MIN_BRIGHTNESS] * 255.0f);
    s->hue_spread = (uint16_t)p[RLEVEL_P_HUE_SPREAD];
}

static void process(void *st, audio_frame_t *f)
{
    rlevel_state_t *s = st;
    int32_t lvl = f->level;
    uint8_t bri;

    if (s->dimming) {
        uint8_t v = led_gamma8((uint8_t)(lvl >> 7));
        bri = v > s->min_bri ? v : s->min_bri;
    } else {
        if (!s->on && lvl > s->thr) {
            s->on = true;
            s->hold = s->hold_blocks;
        } else if (s->on && s->hold == 0 && lvl < s->thr - s->hyst) {
            s->on = false;
        }
        if (s->hold) s->hold--;
        bri = s->on ? 255 : s->min_bri;
    }
    uint16_t hue = (uint16_t)(s->hue + (((uint32_t)s->hue_spread * (uint32_t)lvl) >> 15));
    led_fill(f->leds, f->num_leds, led_hsv(hue, s->sat, bri));
}

const stage_def_t g_stage_render_level = {
    .id = STAGE_RENDER_LEVEL,
    .name = "Render: level",
    .desc = "All LEDs on/off (or dimmed) by the normalised level",
    .params = s_params,
    .num_params = RLEVEL_P_COUNT,
    .state = &s_st,
    .reset = reset,
    .configure = configure,
    .process = process,
};
