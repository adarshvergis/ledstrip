/**
 * @file hw_common.h
 * @brief Small register-level helpers shared by the drivers (GPIO matrix, clock
 *        gating, interrupt hookup, DMA descriptors, cycle counter, CRC).
 */
#ifndef HW_COMMON_H
#define HW_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HW_CPU_HZ           160000000u

/** Generic return codes used by all Interface modules. */
typedef enum {
    HW_OK = 0,
    HW_ERR_ARG = -1,
    HW_ERR_STATE = -2,
    HW_ERR_NO_MEM = -3,
    HW_ERR_TIMEOUT = -4,
    HW_ERR_IO = -5,
    HW_ERR_NOT_FOUND = -6,
    HW_ERR_CRC = -7,
    HW_ERR_BUSY = -8,
} hw_err_t;

/** GDMA linked-list descriptor (layout fixed by hardware, ESP32-C3 TRM "GDMA"). */
typedef struct hw_dma_desc {
    volatile uint32_t size   : 12;  /**< buffer size in bytes                    */
    volatile uint32_t length : 12;  /**< valid bytes (TX: to send, RX: received) */
    volatile uint32_t rsvd   : 4;
    volatile uint32_t err_eof: 1;
    volatile uint32_t rsvd2  : 1;
    volatile uint32_t suc_eof: 1;   /**< last descriptor of a frame              */
    volatile uint32_t owner  : 1;   /**< 1 = owned by DMA                        */
    void *buffer;
    struct hw_dma_desc *next;
} hw_dma_desc_t;

/** GDMA channel allocation (IDF drivers allocate from channel 0 upwards). */
#define HW_GDMA_CH_LED      1       /**< SPI2 TX -> WS2812B                     */
#define HW_GDMA_CH_MIC      2       /**< I2S RX  <- INMP441                     */

/**
 * Write a register bit-field with a full 32-bit read-modify-write. Required for
 * fields that are 8/16-bit aligned, which GCC may otherwise access with byte/half
 * stores that the peripheral bus does not support.
 */
#define HW_REG_SET_FIELD(reg, field, value)     \
    do {                                        \
        __typeof__(reg) _t;                     \
        _t.val = (reg).val;                     \
        _t.field = (value);                     \
        (reg).val = _t.val;                     \
    } while (0)

/** Read the CPU cycle counter (ESP32-C3 machine performance counter CSR 0x7E2). */
static inline uint32_t hw_cycles(void)
{
    uint32_t c;
    __asm__ volatile("csrr %0, 0x7e2" : "=r"(c));
    return c;
}

/** Convert CPU cycles to microseconds. */
static inline uint32_t hw_cycles_to_us(uint32_t cycles)
{
    return cycles / (HW_CPU_HZ / 1000000u);
}

/** Route a peripheral output signal to a GPIO through the GPIO matrix. */
void hw_gpio_route_out(uint32_t gpio, uint32_t signal_idx);

/** Route a GPIO to a peripheral input signal through the GPIO matrix. */
void hw_gpio_route_in(uint32_t gpio, uint32_t signal_idx);

/** Detach a GPIO from the matrix and make it a high-impedance input. */
void hw_gpio_release(uint32_t gpio);

/** Peripheral clock gates handled by hw_periph_enable(). */
typedef enum {
    HW_PERIPH_I2S,
    HW_PERIPH_SPI2,
    HW_PERIPH_GDMA,
} hw_periph_t;

/** Enable the bus clock of a peripheral and pulse its reset. */
void hw_periph_enable(hw_periph_t periph);

/** Gate the bus clock of a peripheral. */
void hw_periph_disable(hw_periph_t periph);

/** Interrupt handler signature (must live in IRAM). */
typedef void (*hw_isr_t)(void *arg);

/**
 * Attach an IRAM interrupt handler to a peripheral interrupt source
 * (ETS_xxx_INTR_SOURCE). The IDF allocator is used for vector setup only.
 */
int hw_intr_attach(int source, hw_isr_t handler, void *arg);

/** CRC-32 (ISO-HDLC, same as zlib.crc32). Pass 0 as @p crc for the first block. */
uint32_t hw_crc32(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HW_COMMON_H */
