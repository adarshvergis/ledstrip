/**
 * @file stage_params.h
 * @brief Registry of processing stage ids and their parameter enums.
 *
 * Stage ids and parameter numbers are persistent keys (calib store key =
 * (stage << 8) | param) and part of the host protocol: NEVER renumber existing
 * entries. To add a stage: append an id before STAGE_COUNT, add its parameter enum
 * here, implement it in stages/, list it in pipeline.c (s_all_stages) and use it in
 * an algorithm in algorithms.c.
 */
#ifndef STAGE_PARAMS_H
#define STAGE_PARAMS_H

typedef enum {
    STAGE_FRONTEND             = 0,     /**< channel select, DC removal, input gain     */
    STAGE_FILTER_A             = 1,     /**< configurable biquad (default: high-pass)   */
    STAGE_FILTER_B             = 2,     /**< configurable biquad (default: off)         */
    STAGE_ENVELOPE             = 3,     /**< amplitude envelope follower                */
    STAGE_AGC                  = 4,     /**< level normalisation -> 0..1                */
    STAGE_SPECTRUM             = 5,     /**< FFT, log bands, spectral centroid          */
    STAGE_BEAT                 = 6,     /**< bass onset detection + tempo tracking      */
    STAGE_RETIRED_7            = 7,     /**< was "render level" (amplitude): never reuse */
    STAGE_RENDER_BEAT_FLASH    = 8,     /**< all LEDs on the beat                       */
    STAGE_RENDER_SPECTRUM      = 9,     /**< one bar per frequency band                 */
    STAGE_RENDER_BEAT_SEGMENTS = 10,    /**< beat pulses placed by spectral centroid    */
    STAGE_MELODY               = 11,    /**< lead voice pitch / note tracker            */
    STAGE_MUSIC                = 12,    /**< hits, scenes (groove/lead/fill...), genre  */
    STAGE_RENDER_SHOW          = 13,    /**< scene-aware layered light show             */
    STAGE_COUNT
} stage_id_t;

/* ---- STAGE_FRONTEND ---- */
typedef enum {
    FRONTEND_P_GAIN_DB = 0,
    FRONTEND_P_DC_CUTOFF_HZ,
    FRONTEND_P_MIC_CHANNEL,             /**< enum auto|left|right */
    FRONTEND_P_COUNT
} frontend_param_t;

/* ---- STAGE_FILTER_A / STAGE_FILTER_B ---- */
typedef enum {
    FILTER_P_TYPE = 0,                  /**< enum off|lowpass|highpass|bandpass|notch|peak|lowshelf|highshelf */
    FILTER_P_FREQ_HZ,
    FILTER_P_Q,
    FILTER_P_GAIN_DB,
    FILTER_P_COUNT
} filter_param_t;

/* ---- STAGE_ENVELOPE ---- */
typedef enum {
    ENVELOPE_P_ATTACK_MS = 0,
    ENVELOPE_P_RELEASE_MS,
    ENVELOPE_P_DETECTOR,                /**< enum rms|peak */
    ENVELOPE_P_COUNT
} envelope_param_t;

/* ---- STAGE_AGC ---- */
typedef enum {
    AGC_P_PEAK_DECAY_DB_S = 0,
    AGC_P_GATE_DB,
    AGC_P_MAX_GAIN_DB,
    AGC_P_HEADROOM_DB,
    AGC_P_CURVE,                        /**< enum log|linear */
    AGC_P_RANGE_DB,
    AGC_P_COUNT
} agc_param_t;

/* ---- STAGE_SPECTRUM ---- */
typedef enum {
    SPECTRUM_P_MIN_FREQ_HZ = 0,
    SPECTRUM_P_MAX_FREQ_HZ,
    SPECTRUM_P_RANGE_DB,
    SPECTRUM_P_FLOOR_DB,
    SPECTRUM_P_ATTACK_MS,
    SPECTRUM_P_RELEASE_MS,
    SPECTRUM_P_NORMALIZE,               /**< bool */
    SPECTRUM_P_NORM_DECAY_DB_S,
    SPECTRUM_P_COUNT
} spectrum_param_t;

/* ---- STAGE_BEAT ---- */
typedef enum {
    BEAT_P_BASS_CUTOFF_HZ = 0,
    BEAT_P_SENSITIVITY,
    BEAT_P_MIN_ONSET_DB,
    BEAT_P_MIN_BPM,
    BEAT_P_MAX_BPM,
    BEAT_P_TEMPO_LOCK,                  /**< bool */
    BEAT_P_LOCK_TOLERANCE,
    BEAT_P_FLYWHEEL_BEATS,
    BEAT_P_COUNT
} beat_param_t;

/* ---- STAGE_RENDER_BEAT_FLASH ---- */
typedef enum {
    RBEAT_P_MODE = 0,                   /**< enum on/off|flash */
    RBEAT_P_ON_TIME_MS,
    RBEAT_P_DECAY_MS,
    RBEAT_P_HUE,
    RBEAT_P_SATURATION,
    RBEAT_P_HUE_STEP,
    RBEAT_P_USE_STRENGTH,               /**< bool */
    RBEAT_P_FLOOR,
    RBEAT_P_COUNT
} rbeat_param_t;

/* ---- STAGE_RENDER_SPECTRUM ---- */
typedef enum {
    RSPEC_P_LAYOUT = 0,                 /**< enum segments|centered|mirror */
    RSPEC_P_HUE_LOW,
    RSPEC_P_HUE_HIGH,
    RSPEC_P_GAIN,
    RSPEC_P_THRESHOLD,
    RSPEC_P_PEAK_HOLD_MS,
    RSPEC_P_PEAK_FALL,
    RSPEC_P_FADE,                       /**< bool */
    RSPEC_P_COUNT
} rspec_param_t;

/* ---- STAGE_RENDER_BEAT_SEGMENTS ---- */
typedef enum {
    RSEG_P_MIN_WIDTH = 0,
    RSEG_P_MAX_WIDTH,
    RSEG_P_DECAY_MS,
    RSEG_P_POSITION,                    /**< enum centroid|dominant band|sweep */
    RSEG_P_HUE_MODE,                    /**< enum position|beat cycle|fixed */
    RSEG_P_HUE,
    RSEG_P_SATURATION,
    RSEG_P_MAX_PULSES,
    RSEG_P_SPREAD,                      /**< bool */
    RSEG_P_COUNT
} rseg_param_t;

/* ---- STAGE_MELODY ---- */
typedef enum {
    MELODY_P_MIN_FREQ_HZ = 0,
    MELODY_P_MAX_FREQ_HZ,
    MELODY_P_SALIENCE,                  /**< tonality threshold for "a voice is playing" */
    MELODY_P_NOTE_MIN_MS,
    MELODY_P_GLIDE_MS,
    MELODY_P_REATTACK_DB,
    MELODY_P_COUNT
} melody_param_t;

/* ---- STAGE_MUSIC ---- */
typedef enum {
    MUSIC_P_GENRE = 0,                  /**< enum auto|edm|pop|rock|metal|rnb|hiphop|trap|orchestral */
    MUSIC_P_CLARITY,
    MUSIC_P_LEAD_SENS,
    MUSIC_P_FILL_SENS,
    MUSIC_P_HIT_DB,
    MUSIC_P_FADE_MS,
    MUSIC_P_DROP_DETECT,                /**< bool */
    MUSIC_P_DROP_BARS,
    MUSIC_P_BEATS_PER_BAR,
    MUSIC_P_COUNT
} music_param_t;

/* ---- STAGE_RENDER_SHOW ---- */
typedef enum {
    RSHOW_P_PALETTE = 0,                /**< enum genre|custom */
    RSHOW_P_HUE,
    RSHOW_P_SATURATION,
    RSHOW_P_FULL_BIAS,
    RSHOW_P_ON_TIME_MS,                 /**< 0 = genre profile */
    RSHOW_P_LEAD_TRAIL_MS,
    RSHOW_P_SPARKLE,
    RSHOW_P_AMBIENT,
    RSHOW_P_MAX_SPARKS,
    RSHOW_P_COUNT
} rshow_param_t;

#endif /* STAGE_PARAMS_H */
