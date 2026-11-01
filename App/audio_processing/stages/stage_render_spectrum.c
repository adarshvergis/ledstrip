/**
 * @file stage_render_spectrum.c
 * @brief Render: the strip is split into one segment per frequency band; the number
 *        of lit LEDs in a segment follows that band's level (with peak markers).
 *
 * Layouts: segments (bar grows from segment start), centered (bar grows from the
 * segment centre), mirror (half strip mirrored, bass in the middle).
 */
#include "audio_types.h"
#include "led_color.h"

#include <string.h>

#define NB  ((int)AUDIO_NUM_BANDS)

typedef struct {
    uint8_t  layout;
    uint16_t hue_lo, hue_hi;
    int32_t  gain_q8;
    int32_t  thr;               /* Q15 */
    uint16_t hold_blocks;
    int32_t  fall_q8;           /* LEDs (Q8) per block */
    bool     fade;
    /* runtime */
    int32_t  peak_q8[NB];       /* in units of "fraction of segment" * 256 * 256 */
    uint16_t hold[NB];
    led_rgb_t tmp[LED_MAX_COUNT];
} rspec_state_t;

static rspec_state_t s_st;

static const param_desc_t s_params[RSPEC_P_COUNT] = {
    [RSPEC_P_LAYOUT]       = PDESC_E("layout", "segments|centered|mirror", 3, 0.0f),
    [RSPEC_P_HUE_LOW]      = PDESC_I("hue_low", "deg", 0.0f, 359.0f, 0.0f),
    [RSPEC_P_HUE_HIGH]     = PDESC_I("hue_high", "deg", 0.0f, 359.0f, 270.0f),
    [RSPEC_P_GAIN]         = PDESC_F("gain", "x", 0.25f, 4.0f, 1.0f, 0.05f),
    [RSPEC_P_THRESHOLD]    = PDESC_F("threshold", "", 0.0f, 0.5f, 0.08f, 0.01f),
    [RSPEC_P_PEAK_HOLD_MS] = PDESC_I("peak_hold", "ms", 0.0f, 2000.0f, 400.0f),
    [RSPEC_P_PEAK_FALL]    = PDESC_F("peak_fall", "seg/s", 0.2f, 20.0f, 2.5f, 0.1f),
    [RSPEC_P_FADE]         = PDESC_B("fade", 1.0f),
};

static void reset(void *st)
{
    rspec_state_t *s = st;
    memset(s->peak_q8, 0, sizeof(s->peak_q8));
    memset(s->hold, 0, sizeof(s->hold));
}

static void configure(void *st, const float *p)
{
    rspec_state_t *s = st;
    s->layout = (uint8_t)(p[RSPEC_P_LAYOUT] + 0.5f);
    s->hue_lo = (uint16_t)p[RSPEC_P_HUE_LOW];
    s->hue_hi = (uint16_t)p[RSPEC_P_HUE_HIGH];
    s->gain_q8 = (int32_t)(p[RSPEC_P_GAIN] * 256.0f);
    s->thr = (int32_t)(p[RSPEC_P_THRESHOLD] * 32767.0f);
    s->hold_blocks = (uint16_t)(p[RSPEC_P_PEAK_HOLD_MS] / BLOCK_MS);
    s->fall_q8 = (int32_t)(p[RSPEC_P_PEAK_FALL] * 65536.0f / BLOCK_RATE_HZ);
    s->fade = p[RSPEC_P_FADE] > 0.5f;
}

/* Draw all bands as bars into dst[0..n). Heights are fractions (Q16) of a segment. */
static void draw_segments(rspec_state_t *s, const int32_t *h_q16, led_rgb_t *dst, uint16_t n, bool centered)
{
    for (int b = 0; b < NB; b++) {
        uint16_t start = (uint16_t)((uint32_t)b * n / NB);
        uint16_t end = (uint16_t)((uint32_t)(b + 1) * n / NB);
        uint16_t len = end - start;
        if (len == 0) continue;
        int32_t hue = s->hue_lo + ((int32_t)(s->hue_hi - s->hue_lo) * b) / (NB > 1 ? NB - 1 : 1);
        if (hue < 0) hue += 360;
        int32_t lit_q8 = (int32_t)(((int64_t)h_q16[b] * len) >> 8);      /* LEDs, Q8 */
        int32_t pk_q8 = (int32_t)(((int64_t)s->peak_q8[b] * len) >> 8);
        for (uint16_t j = 0; j < len; j++) {
            /* position of LED j measured from where the bar starts */
            int32_t pos;
            if (centered) {
                int32_t c2 = (int32_t)len - 1;                  /* 2 * centre */
                int32_t d2 = (int32_t)(2 * j) - c2;
                pos = (d2 < 0 ? -d2 : d2) * 128;                /* distance from centre, Q8 */
                pos *= 2;                                       /* both halves share the bar */
            } else {
                pos = j * 256;
            }
            int32_t v = lit_q8 - pos;
            uint8_t bri = v >= 256 ? 255 : (v <= 0 ? 0 : (uint8_t)v);
            if (bri && s->fade) {
                bri = led_scale8(bri, (uint8_t)(110u + (145u * (uint32_t)j) / len));
            }
            led_rgb_t c = led_hsv((uint16_t)hue, 255, led_gamma8(bri));
            /* peak marker */
            if (s->peak_q8[b] > 0 && pk_q8 >= pos && pk_q8 < pos + 256 && !centered) {
                c = led_max(c, (led_rgb_t){90, 90, 90});
            }
            dst[start + j] = c;
        }
    }
}

static void process(void *st, audio_frame_t *f)
{
    rspec_state_t *s = st;
    int32_t h[NB];
    for (int b = 0; b < NB; b++) {
        int32_t v = f->bands[b];
        v = (v < s->thr) ? 0 : (int32_t)(((int64_t)(v - s->thr) * 32767) / (32767 - s->thr));
        v = (v * s->gain_q8) >> 8;
        if (v > 32767) v = 32767;
        h[b] = v * 2;                                   /* Q15 -> Q16 */
        if (h[b] >= s->peak_q8[b]) {
            s->peak_q8[b] = h[b];
            s->hold[b] = s->hold_blocks;
        } else if (s->hold[b]) {
            s->hold[b]--;
        } else {
            s->peak_q8[b] -= s->fall_q8;
            if (s->peak_q8[b] < 0) s->peak_q8[b] = 0;
        }
    }

    uint16_t n = f->num_leds;
    if (s->layout == 2) {
        uint16_t half = (uint16_t)((n + 1) / 2);
        draw_segments(s, h, s->tmp, half, false);
        for (uint16_t i = 0; i < half; i++) {
            /* tmp[0] = bass -> centre of the strip */
            uint16_t left = (uint16_t)(half - 1 - i);
            uint16_t right = (uint16_t)(n - half + i);
            f->leds[left] = s->tmp[i];
            f->leds[right] = s->tmp[i];
        }
    } else {
        draw_segments(s, h, f->leds, n, s->layout == 1);
    }
}

const stage_def_t g_stage_render_spectrum = {
    .id = STAGE_RENDER_SPECTRUM,
    .name = "Render: spectrum bars",
    .desc = "Segment per band (which LEDs), bar length = band level (how many LEDs)",
    .params = s_params,
    .num_params = RSPEC_P_COUNT,
    .state = &s_st,
    .reset = reset,
    .configure = configure,
    .process = process,
};
