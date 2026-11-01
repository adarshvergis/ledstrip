/**
 * @file pipeline.h
 * @brief Multi-stage audio processing pipeline and algorithm registry.
 *
 * Threading: pipeline_process() runs in the real-time task. Other tasks only post
 * requests (algorithm change, "parameters of stage X changed", sample capture);
 * these are applied at the start of the next block, so stages never see
 * parameters change in the middle of a block and no locks are needed in the hot path.
 */
#ifndef PIPELINE_H
#define PIPELINE_H

#include <stdint.h>
#include <stdbool.h>
#include "audio_types.h"

#define PIPELINE_CAPTURE_MAX    256     /**< max samples per capture request */
#define PIPELINE_CAPTURE_INPUT  (-1)    /**< capture before the first stage  */

/** Table of all algorithms (index = algorithm id). */
extern const algorithm_def_t g_algorithms[NUM_ALGORITHMS];

/** Initialise all stages (configure from current calibration values). */
void pipeline_init(uint8_t algorithm);

/** Request a switch to @p algorithm (applied before the next block). */
int pipeline_select(uint8_t algorithm);

uint8_t pipeline_active(void);

/** Notify that parameters of @p stage changed (from any task). */
void pipeline_params_changed(uint8_t stage);

/** Process one block (real-time task only). */
void pipeline_process(audio_frame_t *f);

/** Stage definition by stage id (NULL if unknown). */
const stage_def_t *pipeline_stage(uint8_t stage_id);

/** All known stage ids, in id order. Returns count. */
uint8_t pipeline_stage_ids(uint8_t *out, uint8_t max);

/**
 * Capture @p n samples as seen after stage index @p stage_index of the active
 * algorithm (PIPELINE_CAPTURE_INPUT = raw input). Returns HW_ERR_BUSY if a capture
 * is already running.
 */
int pipeline_capture_start(int8_t stage_index, uint16_t n);

/** True when the requested capture is complete. */
bool pipeline_capture_done(void);

/** Captured samples (valid once done). */
const q31_t *pipeline_capture_data(uint16_t *n);

/** Average / max execution time of stage @p idx of the active algorithm [cycles]. */
void pipeline_stage_timing(uint8_t idx, uint32_t *avg, uint32_t *max);

/** Reset timing statistics. */
void pipeline_reset_timing(void);

#endif /* PIPELINE_H */
