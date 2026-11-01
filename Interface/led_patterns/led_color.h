/**
 * @file led_color.h
 * @brief Integer colour helpers (HSV, scaling, blending, gamma, palettes).
 */
#ifndef LED_COLOR_H
#define LED_COLOR_H

#include <stdint.h>
#include "ws2812.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LED_BLACK   ((led_rgb_t){0, 0, 0})

/** v * s / 255 (rounded so that 255 * 255 = 255). */
static inline uint8_t led_scale8(uint8_t v, uint8_t s)
{
    return (uint8_t)(((uint16_t)v * (uint16_t)(s + 1u)) >> 8);
}

/** HSV -> RGB. @p hue 0..359 degrees (wraps), @p sat / @p val 0..255. */
led_rgb_t led_hsv(uint16_t hue, uint8_t sat, uint8_t val);

led_rgb_t led_scale(led_rgb_t c, uint8_t s);

/** Linear blend: t = 0 -> a, t = 255 -> b. */
led_rgb_t led_blend(led_rgb_t a, led_rgb_t b, uint8_t t);

/** Saturating add. */
led_rgb_t led_add(led_rgb_t a, led_rgb_t b);

/** Per-channel maximum. */
led_rgb_t led_max(led_rgb_t a, led_rgb_t b);

void led_fill(led_rgb_t *fb, uint16_t n, led_rgb_t c);

/** Perceptual gamma (2.2) for an 8-bit intensity. */
uint8_t led_gamma8(uint8_t v);

/** Sample a palette at @p pos (0..65535 spans the palette, wraps to the first entry). */
led_rgb_t led_palette(const led_rgb_t *pal, uint8_t len, uint16_t pos);

#ifdef __cplusplus
}
#endif

#endif /* LED_COLOR_H */
