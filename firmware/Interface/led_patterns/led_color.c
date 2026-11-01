/**
 * @file led_color.c
 * @brief Integer colour helpers.
 */
#include "led_color.h"

static const uint8_t s_gamma[256] = {
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,
      1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,   2,   2,
      3,   3,   3,   3,   3,   4,   4,   4,   4,   5,   5,   5,   5,   6,   6,   6,
      6,   7,   7,   7,   8,   8,   8,   9,   9,   9,  10,  10,  11,  11,  11,  12,
     12,  13,  13,  13,  14,  14,  15,  15,  16,  16,  17,  17,  18,  18,  19,  19,
     20,  20,  21,  22,  22,  23,  23,  24,  25,  25,  26,  26,  27,  28,  28,  29,
     30,  30,  31,  32,  33,  33,  34,  35,  35,  36,  37,  38,  39,  39,  40,  41,
     42,  43,  43,  44,  45,  46,  47,  48,  49,  49,  50,  51,  52,  53,  54,  55,
     56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,
     73,  74,  75,  76,  77,  78,  79,  81,  82,  83,  84,  85,  87,  88,  89,  90,
     91,  93,  94,  95,  97,  98,  99, 100, 102, 103, 105, 106, 107, 109, 110, 111,
    113, 114, 116, 117, 119, 120, 121, 123, 124, 126, 127, 129, 130, 132, 133, 135,
    137, 138, 140, 141, 143, 145, 146, 148, 149, 151, 153, 154, 156, 158, 159, 161,
    163, 165, 166, 168, 170, 172, 173, 175, 177, 179, 181, 182, 184, 186, 188, 190,
    192, 194, 196, 197, 199, 201, 203, 205, 207, 209, 211, 213, 215, 217, 219, 221,
    223, 225, 227, 229, 231, 234, 236, 238, 240, 242, 244, 246, 248, 251, 253, 255,
};

led_rgb_t led_hsv(uint16_t hue, uint8_t sat, uint8_t val)
{
    hue %= 360u;
    uint8_t region = (uint8_t)(hue / 60u);
    uint16_t rem = (uint16_t)((hue % 60u) * 255u / 60u);   /* 0..255 within region */
    uint8_t p = led_scale8(val, (uint8_t)(255u - sat));
    uint8_t q = led_scale8(val, (uint8_t)(255u - led_scale8(sat, (uint8_t)rem)));
    uint8_t t = led_scale8(val, (uint8_t)(255u - led_scale8(sat, (uint8_t)(255u - rem))));
    switch (region) {
    case 0:  return (led_rgb_t){val, t, p};
    case 1:  return (led_rgb_t){q, val, p};
    case 2:  return (led_rgb_t){p, val, t};
    case 3:  return (led_rgb_t){p, q, val};
    case 4:  return (led_rgb_t){t, p, val};
    default: return (led_rgb_t){val, p, q};
    }
}

led_rgb_t led_scale(led_rgb_t c, uint8_t s)
{
    return (led_rgb_t){led_scale8(c.r, s), led_scale8(c.g, s), led_scale8(c.b, s)};
}

led_rgb_t led_blend(led_rgb_t a, led_rgb_t b, uint8_t t)
{
    uint8_t it = (uint8_t)(255u - t);
    return (led_rgb_t){
        (uint8_t)(led_scale8(a.r, it) + led_scale8(b.r, t)),
        (uint8_t)(led_scale8(a.g, it) + led_scale8(b.g, t)),
        (uint8_t)(led_scale8(a.b, it) + led_scale8(b.b, t)),
    };
}

static inline uint8_t add_sat(uint8_t a, uint8_t b)
{
    uint16_t s = (uint16_t)a + b;
    return (uint8_t)(s > 255u ? 255u : s);
}

led_rgb_t led_add(led_rgb_t a, led_rgb_t b)
{
    return (led_rgb_t){add_sat(a.r, b.r), add_sat(a.g, b.g), add_sat(a.b, b.b)};
}

led_rgb_t led_max(led_rgb_t a, led_rgb_t b)
{
    return (led_rgb_t){a.r > b.r ? a.r : b.r, a.g > b.g ? a.g : b.g, a.b > b.b ? a.b : b.b};
}

void led_fill(led_rgb_t *fb, uint16_t n, led_rgb_t c)
{
    for (uint16_t i = 0; i < n; i++) {
        fb[i] = c;
    }
}

uint8_t led_gamma8(uint8_t v)
{
    return s_gamma[v];
}

led_rgb_t led_palette(const led_rgb_t *pal, uint8_t len, uint16_t pos)
{
    if (len == 0) return LED_BLACK;
    if (len == 1) return pal[0];
    uint32_t scaled = (uint32_t)pos * len;          /* wrap: last entry blends to first */
    uint8_t i = (uint8_t)(scaled >> 16);
    uint8_t frac = (uint8_t)(scaled >> 8);
    return led_blend(pal[i], pal[(i + 1u) % len], frac);
}
