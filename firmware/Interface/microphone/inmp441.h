/**
 * @file inmp441.h
 * @brief INMP441 I2S MEMS microphone driver (register-level I2S RX + GDMA ring).
 *
 * The I2S peripheral runs as master in Philips standard mode, 32-bit slots, both
 * slots captured (stereo). DMA continuously fills a ring of fixed-size blocks; when a
 * block is complete the user callback is invoked from interrupt context with the block
 * index. Sample words are 32-bit, left aligned 24-bit two's complement data
 * (low 8 bits zero). Frame layout in a block: [slot0, slot1, slot0, slot1, ...].
 */
#ifndef INMP441_H
#define INMP441_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Called from the DMA interrupt (IRAM) when block @p block_idx has been filled. */
typedef void (*mic_block_cb_t)(uint32_t block_idx, void *arg);

typedef struct {
    uint8_t  pin_sck;           /**< bit clock output                 */
    uint8_t  pin_ws;            /**< word select output               */
    uint8_t  pin_sd;            /**< serial data input                */
    uint32_t sample_rate;       /**< Hz                               */
    uint16_t frames_per_block;  /**< stereo frames per DMA block      */
    uint8_t  num_blocks;        /**< blocks in the DMA ring (>= 2)    */
} mic_config_t;

/** Configure pins, clocks, I2S and DMA. Does not start capturing. */
int mic_init(const mic_config_t *cfg, mic_block_cb_t cb, void *arg);

/** Start capturing (resets the ring). */
int mic_start(void);

/** Stop capturing; the I2S clocks stop (microphone idle). */
void mic_stop(void);

bool mic_is_running(void);

/** Pointer to the interleaved samples of ring block @p idx (frames_per_block*2 words). */
const int32_t *mic_block(uint32_t idx);

/** Actual sample rate produced by the fractional clock divider [Hz]. */
uint32_t mic_actual_sample_rate(void);

/** Number of blocks completed since mic_start(). */
uint32_t mic_blocks_completed(void);

#ifdef __cplusplus
}
#endif

#endif /* INMP441_H */
