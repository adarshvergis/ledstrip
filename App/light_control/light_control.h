/**
 * @file light_control.h
 * @brief LED output stage: frame buffer, overlays, brightness, power limiting.
 *
 * Producers (algorithms, patterns, system indications) draw into the frame buffer;
 * light_show() composes the output (CLI test override, error indication), applies the
 * global brightness and the current budget, and starts the DMA transfer.
 */
#ifndef LIGHT_CONTROL_H
#define LIGHT_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include "ws2812.h"

typedef struct {
    uint32_t frames;
    uint32_t limited_frames;    /**< frames scaled down by the power limiter */
    uint32_t est_current_ma;    /**< estimate for the last frame */
    uint32_t busy_errors;
} light_stats_t;

/** Initialise the WS2812 driver. */
int light_init(void);

/** Frame buffer to render into (light_num_leds() entries). */
led_rgb_t *light_framebuffer(void);

uint16_t light_num_leds(void);

/** Compose overlays, apply brightness / power limit and send to the strip. */
void light_show(void);

void light_set_brightness(uint8_t b);
uint8_t light_brightness(void);
void light_set_max_current(uint16_t ma);
void light_set_num_leds(uint16_t n);
void light_set_color_order(uint8_t order);

/** Copy of the last frame actually sent (for the host live view). */
uint16_t light_get_output(led_rgb_t *out, uint16_t max);

/** CLI test: colour LEDs [start, end] for @p hold_ms (0 = until released). */
void light_test_set(led_rgb_t c, uint16_t start, uint16_t end, uint32_t hold_ms);
void light_test_release(void);

void light_get_stats(light_stats_t *st);

#endif /* LIGHT_CONTROL_H */
