/**
 * @file mode_manager.h
 * @brief Device mode management and the real-time task.
 *
 * Device modes (no bootloader involvement, switching is instantaneous):
 *   MAIN         - normal operation (audio algorithm or pattern)
 *   CALIBRATION  - same output, but parameters may be changed live and saved
 *   FW_UPDATE    - microphone and algorithm stopped, LEDs show update progress
 *
 * Run modes (persisted setting, selected from the host):
 *   AUDIO   - microphone -> pipeline -> LEDs, block driven (lowest latency)
 *   PATTERN - pre-stored patterns, microphone disabled, 100 Hz frame timer
 *
 * The real-time task is the only code that touches the microphone, the pipeline and
 * the strip; other tasks post requests that it applies between blocks.
 */
#ifndef MODE_MANAGER_H
#define MODE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "build_config.h"

typedef enum {
    DEV_MODE_MAIN = 0,
    DEV_MODE_CALIBRATION = 1,
    DEV_MODE_FW_UPDATE = 2,
} dev_mode_t;

typedef enum {
    RUN_MODE_AUDIO = 0,
    RUN_MODE_PATTERN = 1,
} run_mode_t;

/** Snapshot of the analysis results for the host live view. */
typedef struct {
    uint16_t level;
    uint16_t bpm_x10;
    uint16_t beat_count;
    uint16_t centroid;
    uint16_t bands[AUDIO_NUM_BANDS];
} live_snapshot_t;

typedef struct {
    uint32_t blocks;
    uint32_t proc_avg_us;       /**< pipeline + LED start, average  */
    uint32_t proc_max_us;
    uint32_t latency_avg_us;    /**< DMA block complete -> LED DMA start */
    uint32_t latency_max_us;
    uint32_t overruns;          /**< blocks whose processing exceeded the hop time */
} rt_stats_t;

/** Create the real-time task (initialises microphone, strip output, pipeline). */
void mode_init(uint8_t run_mode, uint8_t algorithm, uint8_t pattern);

dev_mode_t mode_get(void);

/** Switch between MAIN and CALIBRATION. Leaving CALIBRATION discards unsaved params. */
int mode_set(dev_mode_t m);

/** Enter FW_UPDATE: stops audio; returns once the real-time task acknowledged. */
int mode_enter_fw_update(void);

/** Leave FW_UPDATE (back to MAIN). */
void mode_exit_fw_update(void);

/** Apply a run configuration (called by the settings module). */
void mode_apply_run_config(uint8_t run_mode, uint8_t algorithm, uint8_t pattern);

run_mode_t mode_run_mode(void);
uint8_t mode_pattern(void);

void mode_get_live(live_snapshot_t *out);
void mode_get_rt_stats(rt_stats_t *out);
void mode_reset_rt_stats(void);

#endif /* MODE_MANAGER_H */
