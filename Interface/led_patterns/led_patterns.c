/**
 * @file led_patterns.c
 * @brief Constant pattern tables and their renderers.
 */
#include "led_patterns.h"
#include "led_color.h"

/* ---- Palettes (const, in flash) ------------------------------------------ */

static const led_rgb_t PAL_WARM_WHITE[] = {{255, 147, 41}};
static const led_rgb_t PAL_OCEAN[]  = {{0, 20, 80}, {0, 90, 160}, {0, 180, 200}, {40, 230, 255}, {0, 60, 140}};
static const led_rgb_t PAL_FIRE[]   = {{40, 0, 0}, {160, 20, 0}, {255, 80, 0}, {255, 170, 20}, {255, 60, 0}};
static const led_rgb_t PAL_FOREST[] = {{0, 60, 10}, {20, 140, 20}, {120, 200, 30}, {10, 90, 40}};
static const led_rgb_t PAL_PARTY[]  = {{255, 0, 80}, {120, 0, 255}, {0, 120, 255}, {0, 255, 120}, {255, 200, 0}};
static const led_rgb_t PAL_BLUE[]   = {{0, 80, 255}};
static const led_rgb_t PAL_PURPLE[] = {{150, 0, 255}};
static const led_rgb_t PAL_STARS[]  = {{255, 255, 255}, {180, 200, 255}, {255, 220, 160}};

#define PAL(p) (p), (uint8_t)(sizeof(p) / sizeof((p)[0]))

/* ---- User pattern table --------------------------------------------------- */

static const led_pattern_t s_patterns[] = {
    {"Rainbow",         PAT_RAINBOW,        PAL(PAL_PARTY),      6000, 0},
    {"Ocean",           PAT_PALETTE_SCROLL, PAL(PAL_OCEAN),      8000, 0},
    {"Fire",            PAT_PALETTE_SCROLL, PAL(PAL_FIRE),       3000, 0},
    {"Forest",          PAT_PALETTE_SCROLL, PAL(PAL_FOREST),    10000, 0},
    {"Breathing warm",  PAT_BREATHE,        PAL(PAL_WARM_WHITE), 4000, 0},
    {"Comet blue",      PAT_COMET,          PAL(PAL_BLUE),       2500, 12},
    {"Theatre chase",   PAT_CHASE,          PAL(PAL_PARTY),       900, 3},
    {"Twinkle stars",   PAT_TWINKLE,        PAL(PAL_STARS),      1500, 6},
    {"Purple glow",     PAT_BREATHE,        PAL(PAL_PURPLE),     6000, 0},
    {"Warm white",      PAT_SOLID,          PAL(PAL_WARM_WHITE),    0, 0},
};

#define NUM_PATTERNS ((uint8_t)(sizeof(s_patterns) / sizeof(s_patterns[0])))

/* 0..255 triangle-ish sine from a 16-bit phase (smooth enough for LEDs). */
static uint8_t sine8(uint16_t phase)
{
    /* parabola approximation of (1 - cos)/2 */
    uint32_t p = phase >> 7;                    /* 0..511 */
    uint32_t x = (p < 256) ? p : 511u - p;      /* 0..255..0 */
    return (uint8_t)((x * x * (765u - 2u * x)) / (255u * 255u));   /* smoothstep */
}

static uint16_t phase_of(uint32_t t_ms, uint16_t period_ms)
{
    if (period_ms == 0) return 0;
    return (uint16_t)(((t_ms % period_ms) * 65536u) / period_ms);
}

static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

uint8_t led_pattern_count(void)
{
    return NUM_PATTERNS;
}

const char *led_pattern_name(uint8_t idx)
{
    return (idx < NUM_PATTERNS) ? s_patterns[idx].name : "?";
}

void led_pattern_render(uint8_t idx, uint32_t t_ms, led_rgb_t *fb, uint16_t n)
{
    if (idx >= NUM_PATTERNS || n == 0) {
        led_fill(fb, n, LED_BLACK);
        return;
    }
    const led_pattern_t *p = &s_patterns[idx];
    uint16_t ph = phase_of(t_ms, p->period_ms);

    switch (p->type) {
    case PAT_SOLID:
        led_fill(fb, n, p->palette[0]);
        break;

    case PAT_BREATHE: {
        uint8_t b = (uint8_t)(20u + led_scale8(sine8(ph), 235));
        led_fill(fb, n, led_scale(p->palette[0], led_gamma8(b)));
        break;
    }

    case PAT_RAINBOW:
        for (uint16_t i = 0; i < n; i++) {
            uint32_t hue = ((uint32_t)i * 360u / n) + ((uint32_t)ph * 360u >> 16);
            fb[i] = led_hsv((uint16_t)hue, 255, 255);
        }
        break;

    case PAT_PALETTE_SCROLL:
        for (uint16_t i = 0; i < n; i++) {
            uint16_t pos = (uint16_t)(((uint32_t)i * 65536u / n) + ph);
            fb[i] = led_palette(p->palette, p->palette_len, pos);
        }
        break;

    case PAT_CHASE: {
        uint8_t w = p->width ? p->width : 3;
        uint16_t step = (uint16_t)(((uint32_t)ph * w) >> 16);
        uint16_t hue_pos = (uint16_t)(t_ms * 8u);
        for (uint16_t i = 0; i < n; i++) {
            fb[i] = ((i + step) % w == 0) ? led_palette(p->palette, p->palette_len, (uint16_t)(hue_pos + i * 1000u))
                                          : LED_BLACK;
        }
        break;
    }

    case PAT_COMET: {
        uint8_t tail = p->width ? p->width : 8;
        /* head bounces: 0 -> n-1 -> 0 */
        uint32_t span = (uint32_t)(n > 1 ? n - 1 : 1);
        uint32_t pos2 = ((uint32_t)ph * span * 2u) >> 16;
        int32_t head = (int32_t)(pos2 <= span ? pos2 : 2u * span - pos2);
        int dir = (pos2 <= span) ? 1 : -1;
        for (uint16_t i = 0; i < n; i++) {
            int32_t d = (head - (int32_t)i) * dir;          /* distance behind head */
            if (d >= 0 && d <= tail) {
                uint8_t b = (uint8_t)(255u - (uint32_t)d * 255u / (tail + 1u));
                fb[i] = led_scale(p->palette[0], led_gamma8(b));
            } else {
                fb[i] = LED_BLACK;
            }
        }
        break;
    }

    case PAT_TWINKLE: {
        uint32_t slot = t_ms / (p->period_ms ? p->period_ms : 1000u);
        uint8_t density = p->width ? p->width : 6;      /* 1 of 'density' LEDs active */
        for (uint16_t i = 0; i < n; i++) {
            uint32_t h = hash32(i * 2654435761u ^ slot * 40503u);
            if ((h % density) == 0) {
                uint8_t b = sine8(ph);
                fb[i] = led_scale(p->palette[(h >> 8) % p->palette_len], led_gamma8(b));
            } else {
                fb[i] = LED_BLACK;
            }
        }
        break;
    }
    }
}

void led_pattern_error(uint8_t code, bool warning, bool fatal, uint32_t t_ms, led_rgb_t *fb, uint16_t n)
{
    const led_rgb_t on_col = warning ? (led_rgb_t){255, 110, 0} : (led_rgb_t){255, 0, 0};
    bool on = (t_ms % 1000u) < 600u;
    uint16_t cnt = code > 16 ? 16 : code;
    if (cnt > n) cnt = n;
    if (fatal) {
        led_fill(fb, n, (led_rgb_t){24, 0, 0});
    }
    for (uint16_t i = 0; i < cnt; i++) {
        fb[i] = on ? on_col : LED_BLACK;
    }
    /* separator LED so the count is easy to read */
    if (cnt < n) {
        fb[cnt] = LED_BLACK;
    }
}

void led_pattern_progress(uint8_t percent, uint32_t t_ms, led_rgb_t *fb, uint16_t n)
{
    if (percent > 100) percent = 100;
    uint16_t lit = (uint16_t)((uint32_t)percent * n / 100u);
    uint16_t marker = (uint16_t)((t_ms / 40u) % (n ? n : 1));
    for (uint16_t i = 0; i < n; i++) {
        fb[i] = (i < lit) ? (led_rgb_t){0, 120, 30} : (led_rgb_t){0, 0, 10};
    }
    if (marker < n) {
        fb[marker] = led_add(fb[marker], (led_rgb_t){60, 60, 60});
    }
}

bool led_pattern_boot(uint32_t t_ms, led_rgb_t *fb, uint16_t n)
{
    const uint32_t dur = 600u;
    if (t_ms >= dur) {
        led_fill(fb, n, LED_BLACK);
        return false;
    }
    uint16_t head = (uint16_t)(t_ms * n / dur);
    for (uint16_t i = 0; i < n; i++) {
        int32_t d = (int32_t)head - i;
        fb[i] = (d >= 0 && d < 6) ? led_hsv((uint16_t)(200u + i * 2u), 255, (uint8_t)(200u - d * 30u)) : LED_BLACK;
    }
    return true;
}
