/**
 * @file light_control.c
 * @brief Output composition, brightness and current limiting.
 */
#include "light_control.h"

#include <string.h>
#include "build_config.h"
#include "hw_common.h"
#include "led_color.h"
#include "led_patterns.h"
#include "error_handler.h"

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "light";

static led_rgb_t s_fb[LED_MAX_COUNT];
static led_rgb_t s_out[LED_MAX_COUNT];
static uint16_t  s_n = LED_DEFAULT_COUNT;
static uint8_t   s_brightness = LED_DEFAULT_BRIGHTNESS;
static uint16_t  s_max_ma = LED_DEFAULT_MAX_CURRENT_MA;
static uint8_t   s_order = LED_ORDER_GRB;
static light_stats_t s_stats;
static portMUX_TYPE s_out_lock = portMUX_INITIALIZER_UNLOCKED;

/* CLI test override */
static volatile bool s_test_on;
static led_rgb_t s_test_color;
static uint16_t  s_test_start, s_test_end;
static int64_t   s_test_until_us;

int light_init(void)
{
    int r = ws2812_init(PIN_LED_DATA, LED_MAX_COUNT);
    if (r != HW_OK) {
        err_report(ERR_LED_INIT);
        return r;
    }
    memset(s_fb, 0, sizeof(s_fb));
    ws2812_show(s_fb, LED_MAX_COUNT, (led_color_order_t)s_order);   /* all off */
    ESP_LOGI(TAG, "WS2812 on GPIO%d, frame time %lu us", PIN_LED_DATA,
             (unsigned long)ws2812_frame_time_us(LED_MAX_COUNT));
    return HW_OK;
}

led_rgb_t *light_framebuffer(void)
{
    return s_fb;
}

uint16_t light_num_leds(void)
{
    return s_n;
}

void light_set_brightness(uint8_t b)
{
    s_brightness = b;
}

uint8_t light_brightness(void)
{
    return s_brightness;
}

void light_set_max_current(uint16_t ma)
{
    s_max_ma = ma < 100 ? 100 : ma;
}

void light_set_num_leds(uint16_t n)
{
    if (n == 0 || n > LED_MAX_COUNT) return;
    if (n < s_n) {
        /* blank LEDs that are no longer driven */
        memset(&s_fb[n], 0, (LED_MAX_COUNT - n) * sizeof(led_rgb_t));
    }
    s_n = n;
}

void light_set_color_order(uint8_t order)
{
    if (order < LED_ORDER_COUNT) s_order = order;
}

void light_test_set(led_rgb_t c, uint16_t start, uint16_t end, uint32_t hold_ms)
{
    s_test_color = c;
    s_test_start = start;
    s_test_end = end;
    s_test_until_us = hold_ms ? esp_timer_get_time() + (int64_t)hold_ms * 1000 : INT64_MAX;
    s_test_on = true;
}

void light_test_release(void)
{
    s_test_on = false;
}

/* Scale the whole frame so the estimated supply current stays within budget. */
static void power_limit(led_rgb_t *px, uint16_t n)
{
    uint32_t sum = 0;
    for (uint16_t i = 0; i < n; i++) {
        sum += (uint32_t)px[i].r + px[i].g + px[i].b;
    }
    uint32_t idle = (uint32_t)LED_IDLE_MA_PER_LED * LED_MAX_COUNT;
    uint32_t ma = (sum * LED_MA_PER_CHANNEL) / 255u;
    uint32_t budget = s_max_ma > idle ? s_max_ma - idle : 0;
    if (ma > budget) {
        uint8_t scale = (uint8_t)((budget * 255u) / ma);
        for (uint16_t i = 0; i < n; i++) {
            px[i] = led_scale(px[i], scale);
        }
        s_stats.limited_frames++;
        ma = budget;
    }
    s_stats.est_current_ma = ma + idle;
}

void light_show(void)
{
    static led_rgb_t frame[LED_MAX_COUNT];
    const uint16_t n = s_n;
    memcpy(frame, s_fb, n * sizeof(led_rgb_t));
    if (n < LED_MAX_COUNT) {
        memset(&frame[n], 0, (LED_MAX_COUNT - n) * sizeof(led_rgb_t));
    }

    /* brightness first: overlays below are always clearly visible */
    for (uint16_t i = 0; i < n; i++) {
        frame[i] = led_scale(frame[i], s_brightness);
    }

    if (s_test_on) {
        if (esp_timer_get_time() > s_test_until_us) {
            s_test_on = false;
        } else {
            for (uint16_t i = s_test_start; i <= s_test_end && i < n; i++) {
                frame[i] = s_test_color;
            }
        }
    }

    err_code_t e = err_active();
    if (e != ERR_NONE) {
        err_severity_t sev = err_severity(e);
        uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);
        led_pattern_error((uint8_t)e, sev == ERR_SEV_WARNING, sev == ERR_SEV_FATAL || err_fatal_pending(),
                          t, frame, n);
    }

    power_limit(frame, LED_MAX_COUNT);

    if (ws2812_show(frame, LED_MAX_COUNT, (led_color_order_t)s_order) != HW_OK) {
        s_stats.busy_errors++;
        err_report(ERR_LED_TIMEOUT);
    }
    s_stats.frames++;

    portENTER_CRITICAL(&s_out_lock);
    memcpy(s_out, frame, sizeof(s_out));
    portEXIT_CRITICAL(&s_out_lock);
}

uint16_t light_get_output(led_rgb_t *out, uint16_t max)
{
    uint16_t n = s_n < max ? s_n : max;
    portENTER_CRITICAL(&s_out_lock);
    memcpy(out, s_out, n * sizeof(led_rgb_t));
    portEXIT_CRITICAL(&s_out_lock);
    return n;
}

void light_get_stats(light_stats_t *st)
{
    *st = s_stats;
}
