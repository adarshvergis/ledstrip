/**
 * @file ws2812.h
 * @brief WS2812B LED strip driver (register-level GPSPI2 + GDMA, no CPU bit-banging).
 *
 * Each WS2812 bit is encoded as 3 SPI bits at 2.5 MHz (0.4 us each):
 *   '0' = 100 (0.4 us high, 0.8 us low), '1' = 110 (0.8 us high, 0.4 us low).
 * A whole frame is sent by DMA; the CPU only encodes (~30 us for 60 LEDs).
 * Frames are double buffered so encoding never waits for the previous transfer.
 */
#ifndef WS2812_H
#define WS2812_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One pixel, always in R,G,B order in memory. */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_rgb_t;

/** Wire order of the colour bytes. */
typedef enum {
    LED_ORDER_GRB = 0,      /**< WS2812B */
    LED_ORDER_RGB,
    LED_ORDER_BRG,
    LED_ORDER_RBG,
    LED_ORDER_GBR,
    LED_ORDER_BGR,
    LED_ORDER_COUNT
} led_color_order_t;

/** Initialise SPI2/GDMA and route the data signal to @p gpio. */
int ws2812_init(uint32_t gpio, uint16_t max_leds);

/**
 * Encode and transmit @p n pixels. Returns immediately after starting DMA.
 * If the previous frame is still being sent, waits for it (max ~2.5 ms).
 * @return HW_OK, or HW_ERR_BUSY if the previous transfer hung (it is aborted).
 */
int ws2812_show(const led_rgb_t *px, uint16_t n, led_color_order_t order);

/** True while a frame transfer is in progress. */
bool ws2812_busy(void);

/** Duration of one frame on the wire for @p n LEDs [us] (incl. latch time). */
uint32_t ws2812_frame_time_us(uint16_t n);

#ifdef __cplusplus
}
#endif

#endif /* WS2812_H */
