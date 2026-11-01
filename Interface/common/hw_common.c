/**
 * @file hw_common.c
 * @brief Register-level helpers shared by the drivers.
 */
#include "hw_common.h"

#include "soc/soc.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/system_struct.h"
#include "freertos/FreeRTOS.h"
#include "esp_intr_alloc.h"

/* IO_MUX register of GPIO n on the ESP32-C3. */
#define IOMUX_REG(n)    (REG_IO_MUX_BASE + 0x04u + 4u * (n))
_Static_assert(IOMUX_REG(2) == IO_MUX_GPIO2_REG, "IO_MUX layout mismatch");

/* Peripheral clock bits are shared with IDF code: modify them inside a critical
 * section (single core -> interrupts off -> atomic read-modify-write). */
static portMUX_TYPE s_clk_lock = portMUX_INITIALIZER_UNLOCKED;

static void iomux_set_gpio_func(uint32_t gpio, bool input_enable)
{
    uint32_t v = REG_READ(IOMUX_REG(gpio));
    v &= ~(MCU_SEL << MCU_SEL_S);
    v |= (PIN_FUNC_GPIO << MCU_SEL_S);
    v &= ~(FUN_DRV_V << FUN_DRV_S);
    v |= (2u << FUN_DRV_S);                 /* ~20 mA drive strength */
    v &= ~(FUN_PU | FUN_PD);
    if (input_enable) {
        v |= FUN_IE;
    } else {
        v &= ~FUN_IE;
    }
    REG_WRITE(IOMUX_REG(gpio), v);
}

void hw_gpio_route_out(uint32_t gpio, uint32_t signal_idx)
{
    iomux_set_gpio_func(gpio, false);
    __typeof__(GPIO.func_out_sel_cfg[0]) cfg;
    cfg.val = 0;
    cfg.func_sel = signal_idx;
    cfg.oen_sel = 1;                             /* output enable from GPIO_ENABLE reg */
    GPIO.func_out_sel_cfg[gpio].val = cfg.val;
    GPIO.enable_w1ts.enable_w1ts = (1u << gpio);
}

void hw_gpio_route_in(uint32_t gpio, uint32_t signal_idx)
{
    iomux_set_gpio_func(gpio, true);
    GPIO.enable_w1tc.enable_w1tc = (1u << gpio);
    GPIO.func_in_sel_cfg[signal_idx].func_sel = gpio;
    GPIO.func_in_sel_cfg[signal_idx].sig_in_inv = 0;
    GPIO.func_in_sel_cfg[signal_idx].sig_in_sel = 1;  /* through the matrix */
}

void hw_gpio_release(uint32_t gpio)
{
    GPIO.enable_w1tc.enable_w1tc = (1u << gpio);
    GPIO.func_out_sel_cfg[gpio].val = SIG_GPIO_OUT_IDX;
    iomux_set_gpio_func(gpio, true);
}

static void periph_set(hw_periph_t periph, bool on)
{
    portENTER_CRITICAL(&s_clk_lock);
    switch (periph) {
    case HW_PERIPH_I2S:
        SYSTEM.perip_clk_en0.reg_i2s1_clk_en = on;
        if (on) {
            SYSTEM.perip_rst_en0.reg_i2s1_rst = 1;
            SYSTEM.perip_rst_en0.reg_i2s1_rst = 0;
        }
        break;
    case HW_PERIPH_SPI2:
        SYSTEM.perip_clk_en0.reg_spi2_clk_en = on;
        if (on) {
            SYSTEM.perip_rst_en0.reg_spi2_rst = 1;
            SYSTEM.perip_rst_en0.reg_spi2_rst = 0;
        }
        break;
    case HW_PERIPH_GDMA:
        /* GDMA comes out of boot held in reset (SYSTEM_DMA_RST defaults to 1): release
         * it. Never pulse it once released (other channels may be running), never gate. */
        if (on) {
            SYSTEM.perip_clk_en1.reg_dma_clk_en = 1;
            if (SYSTEM.perip_rst_en1.reg_dma_rst) {
                SYSTEM.perip_rst_en1.reg_dma_rst = 0;
            }
        }
        break;
    }
    portEXIT_CRITICAL(&s_clk_lock);
}

void hw_periph_enable(hw_periph_t periph)
{
    periph_set(periph, true);
}

void hw_periph_disable(hw_periph_t periph)
{
    periph_set(periph, false);
}

int hw_intr_attach(int source, hw_isr_t handler, void *arg)
{
    intr_handle_t h;
    esp_err_t e = esp_intr_alloc(source, ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL2, handler, arg, &h);
    return (e == ESP_OK) ? HW_OK : HW_ERR_STATE;
}
