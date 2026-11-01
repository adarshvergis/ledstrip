/**
 * @file audio_types.h
 * @brief Types shared by the processing pipeline and its stages.
 *
 * A pipeline is an ordered list of stages. Every block (AUDIO_HOP_SIZE samples) an
 * audio_frame_t travels through all stages: early stages transform samples[] in
 * place, analysis stages fill the feature fields, render stages draw into leds[].
 * Stages communicate only through this frame, so they can be inserted, removed or
 * reordered freely in algorithms.c.
 */
#ifndef AUDIO_TYPES_H
#define AUDIO_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "build_config.h"
#include "dsp.h"
#include "ws2812.h"
#include "stage_params.h"

/** One processing block and all features derived from it. */
typedef struct {
    /* signal */
    q31_t    samples[AUDIO_HOP_SIZE];   /**< mono, modified in place by filter stages */
    uint16_t n;                         /**< valid samples (AUDIO_HOP_SIZE)           */
    uint32_t block;                     /**< running block counter                    */

    /* amplitude */
    uint32_t envelope;                  /**< linear amplitude, Q31 full scale         */
    int32_t  envelope_db;               /**< dBFS, Q8                                 */
    uint16_t level;                     /**< normalised level 0..32767                */

    /* spectrum */
    bool     spectrum_valid;
    uint16_t bands[AUDIO_NUM_BANDS];    /**< band levels 0..32767                     */
    uint16_t centroid;                  /**< spectral centroid / Nyquist, Q15         */
    uint8_t  dominant_band;

    /* rhythm */
    bool     beat;                      /**< a beat occurs in this block              */
    uint16_t beat_strength;             /**< Q15                                      */
    uint16_t beat_phase;                /**< position inside the beat period, Q15     */
    uint16_t bpm_x10;                   /**< tempo * 10, 0 = unknown                  */
    uint32_t beat_count;
    int32_t  onset;                     /**< onset detection function, Q8 dB          */

    /* output */
    led_rgb_t *leds;                    /**< render target                            */
    uint16_t   num_leds;
} audio_frame_t;

/** Parameter value types (protocol "param type"). */
typedef enum {
    PARAM_FLOAT = 0,
    PARAM_INT = 1,
    PARAM_BOOL = 2,
    PARAM_ENUM = 3,     /**< unit string lists the options: "a|b|c" */
} param_type_t;

/** Static description of one calibratable parameter (param id = array index). */
typedef struct {
    const char *name;
    const char *unit;
    uint8_t     type;
    float       min;
    float       max;
    float       def;
    float       step;
} param_desc_t;

/** A processing stage. Callbacks get the stage's private state pointer. */
typedef struct stage_def {
    uint8_t             id;             /**< stage_id_t                            */
    const char         *name;
    const char         *desc;
    const param_desc_t *params;
    uint8_t             num_params;
    void               *state;
    /** Clear run-time state (called when an algorithm starts). */
    void (*reset)(void *state);
    /** Parameters changed: convert to internal fixed-point form (soft float OK). */
    void (*configure)(void *state, const float *p);
    /** Process one block (real-time context). */
    void (*process)(void *state, audio_frame_t *f);
} stage_def_t;

/** An algorithm = named list of stages. */
typedef struct {
    uint8_t                    id;
    const char                *name;
    const char                *desc;
    const stage_def_t *const  *stages;
    uint8_t                    num_stages;
} algorithm_def_t;

/** Helpers for parameter tables. */
#define PDESC_F(n, u, mn, mx, d, s) {(n), (u), PARAM_FLOAT, (mn), (mx), (d), (s)}
#define PDESC_I(n, u, mn, mx, d)    {(n), (u), PARAM_INT,   (mn), (mx), (d), 1.0f}
#define PDESC_B(n, d)               {(n), "",  PARAM_BOOL,  0.0f, 1.0f, (d), 1.0f}
#define PDESC_E(n, opts, cnt, d)    {(n), (opts), PARAM_ENUM, 0.0f, (float)((cnt) - 1), (d), 1.0f}

#define BLOCK_RATE_HZ   ((float)SAMPLE_RATE / (float)AUDIO_HOP_SIZE)
#define BLOCK_MS        (1000.0f / BLOCK_RATE_HZ)

/* Stage definitions (implemented in stages/). */
extern const stage_def_t g_stage_frontend;
extern const stage_def_t g_stage_filter_a;
extern const stage_def_t g_stage_filter_b;
extern const stage_def_t g_stage_envelope;
extern const stage_def_t g_stage_agc;
extern const stage_def_t g_stage_spectrum;
extern const stage_def_t g_stage_beat;
extern const stage_def_t g_stage_render_level;
extern const stage_def_t g_stage_render_beat_flash;
extern const stage_def_t g_stage_render_spectrum;
extern const stage_def_t g_stage_render_beat_segments;

#endif /* AUDIO_TYPES_H */
