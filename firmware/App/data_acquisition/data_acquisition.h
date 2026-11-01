/**
 * @file data_acquisition.h
 * @brief Microphone data acquisition: DMA block -> mono Q31 samples for the pipeline.
 *
 * Chunked, zero-copy until the final conversion: the microphone driver fills a DMA
 * ring of AUDIO_HOP_SIZE-frame blocks; each completed block wakes the real-time task
 * (task notification from the DMA ISR). If the task falls behind, older blocks are
 * skipped so latency never accumulates.
 */
#ifndef DATA_ACQUISITION_H
#define DATA_ACQUISITION_H

#include <stdint.h>
#include <stdbool.h>
#include "dsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    uint32_t blocks;            /**< blocks delivered            */
    uint32_t dropped;           /**< blocks skipped (overrun)    */
    uint32_t timeouts;
    uint8_t  channel;           /**< MIC_CHANNEL_LEFT / _RIGHT in use */
    bool     auto_detecting;
    bool     signal_ok;
    uint32_t actual_rate;
    uint32_t last_age_us;       /**< DMA block completion -> read */
} daq_stats_t;

/** Configure the microphone; @p consumer is woken for every block. */
int daq_init(TaskHandle_t consumer);

int daq_start(void);
void daq_stop(void);
bool daq_running(void);

/**
 * Wait for the next block and convert it to mono Q31 (AUDIO_HOP_SIZE samples).
 * @return HW_OK or HW_ERR_TIMEOUT.
 */
int daq_read_block(q31_t *out, uint32_t timeout_ms);

/** CPU cycle counter value when the last delivered block completed. */
uint32_t daq_last_block_cycles(void);

/** Select MIC_CHANNEL_LEFT / _RIGHT / _AUTO. */
void daq_set_channel(uint8_t ch);

void daq_get_stats(daq_stats_t *st);

#endif /* DATA_ACQUISITION_H */
