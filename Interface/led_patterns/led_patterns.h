/**
 * @file led_patterns.h
 * @brief Pre-stored constant LED patterns (shown when audio processing is skipped)
 *        and system indication patterns (error codes, update progress, boot).
 *
 * User patterns are described entirely by const tables (type, palette, period);
 * the renderer is a pure function of (pattern, time), so patterns cost no RAM.
 */
#ifndef LED_PATTERNS_H
#define LED_PATTERNS_H

#include <stdint.h>
#include <stdbool.h>
#include "ws2812.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAT_SOLID = 0,          /**< palette[0] everywhere                         */
    PAT_BREATHE,            /**< palette[0], sinusoidal brightness             */
    PAT_RAINBOW,            /**< moving rainbow                                */
    PAT_PALETTE_SCROLL,     /**< palette gradient scrolling along the strip    */
    PAT_CHASE,              /**< theatre chase, every 'width'-th LED           */
    PAT_COMET,              /**< bouncing head with fading tail                */
    PAT_TWINKLE,            /**< deterministic random sparkles over palette    */
} led_pattern_type_t;

typedef struct {
    const char        *name;
    led_pattern_type_t type;
    const led_rgb_t   *palette;
    uint8_t            palette_len;
    uint16_t           period_ms;   /**< duration of one animation cycle */
    uint8_t            width;       /**< type specific (chase spacing, comet tail, density) */
} led_pattern_t;

/** Number of user selectable patterns. */
uint8_t led_pattern_count(void);

/** Name of pattern @p idx (or "?"). */
const char *led_pattern_name(uint8_t idx);

/** Render user pattern @p idx at time @p t_ms into @p fb. */
void led_pattern_render(uint8_t idx, uint32_t t_ms, led_rgb_t *fb, uint16_t n);

/**
 * Error indication: the first min(code,16) LEDs blink (red for errors, amber for
 * warnings). If @p fatal the remaining LEDs glow dim red, otherwise they are left
 * untouched (overlay on the normal output).
 */
void led_pattern_error(uint8_t code, bool warning, bool fatal, uint32_t t_ms, led_rgb_t *fb, uint16_t n);

/** Firmware update progress bar (0..100 %) with a moving marker. */
void led_pattern_progress(uint8_t percent, uint32_t t_ms, led_rgb_t *fb, uint16_t n);

/** Short start-up animation; returns false once finished. */
bool led_pattern_boot(uint32_t t_ms, led_rgb_t *fb, uint16_t n);

#ifdef __cplusplus
}
#endif

#endif /* LED_PATTERNS_H */
