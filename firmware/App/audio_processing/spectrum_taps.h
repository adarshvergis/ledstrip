/**
 * @file spectrum_taps.h
 * @brief Signal taps for the host's live spectrum view.
 *
 * The host chooses up to TAP_MAX_SLOTS points of the pipeline (after a given stage, or at
 * the end). The real-time task only copies each block's samples at those points into a
 * ring buffer (no FFT, lock-free); the comm task computes the spectra at the stream rate.
 * Taps that are not requested cost nothing in the real-time path.
 */
#ifndef SPECTRUM_TAPS_H
#define SPECTRUM_TAPS_H

#include <stdint.h>
#include <stdbool.h>
#include "audio_types.h"

#define TAP_END             15u     /**< tap id "after the last stage" (bit 15 of a tap mask) */
#define TAP_MAX_SLOTS       4u      /**< taps captured at the same time                      */
#define TAP_POINTS          72u     /**< spectrum points per tap (log spaced, >= 1 FFT bin)   */

_Static_assert(STAGE_COUNT <= TAP_END, "stage ids must fit below TAP_END in a 16-bit tap mask");

/** Comm task: select taps (bit n = after stage id n, bit TAP_END = end). Returns the mask kept. */
uint16_t taps_set(uint16_t mask);

/** Currently selected taps (read once per block by the pipeline). */
uint16_t taps_mask(void);

/** Real-time task: append a block of samples to tap @p tap (no-op if not selected). */
void taps_capture(uint8_t tap, const q31_t *x, uint16_t n);

/** Point layout: point i covers FFT bins edges[i] .. edges[i+1]-1 (TAP_POINTS + 1 entries). */
const uint8_t *taps_edges(void);

/**
 * Comm task: spectrum of tap @p tap as TAP_POINTS bytes (value v = LIVE_SPEC_FLOOR_DB + v/2 dBFS).
 * @return false if the tap is not selected or has no recent data (stage not in the algorithm).
 */
bool taps_spectrum(uint8_t tap, uint8_t *out);

#endif /* SPECTRUM_TAPS_H */
