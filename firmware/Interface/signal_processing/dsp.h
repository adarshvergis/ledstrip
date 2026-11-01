/**
 * @file dsp.h
 * @brief Fixed-point signal processing library (the ESP32-C3 has no FPU).
 *
 * Conventions
 *  - Time-domain audio is q31_t (full scale = +/-1.0).
 *  - Biquad coefficients are Q2.30 (range +/-2.0), 64-bit accumulation.
 *  - FFT operates on complex Q15 with 1/2 scaling per stage (output = X[k] / N).
 *  - Functions taking float arguments are design-time helpers (soft float, called
 *    only when parameters change), never in the per-sample path.
 */
#ifndef DSP_H
#define DSP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int16_t q15_t;
typedef int32_t q31_t;

typedef struct {
    int16_t re;
    int16_t im;
} cq15_t;

#define Q31_ONE         0x7FFFFFFF
#define Q15_ONE         0x7FFF
#define DSP_FFT_MAX_LOG2    9u                  /**< largest supported FFT: 512 */
#define DSP_FFT_MAX_SIZE    (1u << DSP_FFT_MAX_LOG2)

/* -------------------------------------------------------------------------- */
/*  Basic fixed-point arithmetic                                              */
/* -------------------------------------------------------------------------- */

static inline q31_t dsp_sat_q31(int64_t v)
{
    if (v > INT32_MAX) return INT32_MAX;
    if (v < INT32_MIN) return INT32_MIN;
    return (q31_t)v;
}

static inline q15_t dsp_sat_q15(int32_t v)
{
    if (v > INT16_MAX) return INT16_MAX;
    if (v < INT16_MIN) return INT16_MIN;
    return (q15_t)v;
}

static inline q31_t dsp_mul_q31(q31_t a, q31_t b)
{
    return (q31_t)(((int64_t)a * b) >> 31);
}

static inline q15_t dsp_mul_q15(q15_t a, q15_t b)
{
    return (q15_t)(((int32_t)a * b) >> 15);
}

static inline uint32_t dsp_abs_q31(q31_t v)
{
    return (v < 0) ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
}

/** float [-1,1) -> Q31 (design time). */
q31_t dsp_float_to_q31(float v);

/** float [-1,1) -> Q15 (design time). */
q15_t dsp_float_to_q15(float v);

/** Linear gain factor -> Q16.16 (design time). */
int32_t dsp_gain_db_to_q16(float gain_db);

/**
 * One-pole smoothing coefficient alpha = 1 - exp(-1 / (tau * rate)) in Q31.
 * @param tau_ms time constant [ms]; @param rate_hz update rate [Hz].
 */
q31_t dsp_tau_to_alpha(float tau_ms, float rate_hz);

/** y += alpha * (x - y)  (one-pole low-pass step, values may be any int32). */
static inline int32_t dsp_smooth(int32_t y, int32_t x, q31_t alpha)
{
    return y + (int32_t)((((int64_t)x - y) * alpha) >> 31);
}

/* -------------------------------------------------------------------------- */
/*  Block utilities                                                           */
/* -------------------------------------------------------------------------- */

/** buf[i] *= gain (Q16.16) with saturation. */
void dsp_gain(q31_t *buf, uint32_t n, int32_t gain_q16);

/** Peak absolute value of a block. */
uint32_t dsp_peak_abs(const q31_t *buf, uint32_t n);

/** RMS of a block (Q31 scale). */
uint32_t dsp_rms(const q31_t *buf, uint32_t n);

/** Sum of squares (Q31 samples >> 16 squared), handy for energies. */
uint64_t dsp_energy(const q31_t *buf, uint32_t n);

/**
 * Remove the baseline (least-squares linear trend incl. offset) from a block.
 * Useful for detrending envelopes / onset functions.
 */
void dsp_baseline_remove(q31_t *buf, uint32_t n);

/** Linear interpolation resampler: maps n_in samples onto n_out samples. */
void dsp_interp_linear(const q31_t *in, uint32_t n_in, q31_t *out, uint32_t n_out);

/** 16-bit xorshift pseudo random generator (deterministic). */
uint16_t dsp_rand16(uint32_t *state);

/* -------------------------------------------------------------------------- */
/*  Logarithms / roots                                                        */
/* -------------------------------------------------------------------------- */

/** Integer square root of a 64-bit value. */
uint32_t dsp_isqrt64(uint64_t x);

/** log2(x) in Q16.16 (x > 0; returns INT32_MIN for 0). */
int32_t dsp_log2_q16(uint64_t x);

/** 10*log10(p) in Q8 (1/256 dB) for a power value. */
int32_t dsp_power_to_db_q8(uint64_t p);

/** 20*log10(a) in Q8 (1/256 dB) for an amplitude value. */
int32_t dsp_amp_to_db_q8(uint64_t a);

/** 2^x for x in Q16.16 (x <= 15), result Q16.16. */
uint32_t dsp_exp2_q16(int32_t x_q16);

/** Linear amplitude factor 10^(dB/20) in Q16.16 for a Q8 dB value (<= +90 dB). */
uint32_t dsp_db_q8_to_lin_q16(int32_t db_q8);

/* -------------------------------------------------------------------------- */
/*  Filters                                                                   */
/* -------------------------------------------------------------------------- */

typedef enum {
    DSP_BQ_BYPASS = 0,
    DSP_BQ_LOWPASS,
    DSP_BQ_HIGHPASS,
    DSP_BQ_BANDPASS,        /**< constant 0 dB peak gain */
    DSP_BQ_NOTCH,
    DSP_BQ_PEAK,
    DSP_BQ_LOWSHELF,
    DSP_BQ_HIGHSHELF,
} dsp_biquad_type_t;

/** Biquad coefficients, Q2.30, normalised so a0 = 1: y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2. */
typedef struct {
    int32_t b0, b1, b2, a1, a2;
    bool bypass;
} dsp_biquad_coef_t;

typedef struct {
    dsp_biquad_coef_t c;
    int32_t x1, x2, y1, y2;
} dsp_biquad_t;

/** RBJ cookbook design (design time). @p q is the quality factor, gain for peak/shelf. */
void dsp_biquad_design(dsp_biquad_coef_t *c, dsp_biquad_type_t type, float fs, float f0, float q, float gain_db);

/** Load externally computed coefficients {b0,b1,b2,a1,a2} in Q2.30. */
void dsp_biquad_set_coefs(dsp_biquad_t *bq, const int32_t coefs_q30[5]);

void dsp_biquad_reset(dsp_biquad_t *bq);

/** Filter a block in place. */
void dsp_biquad_process(dsp_biquad_t *bq, q31_t *buf, uint32_t n);

/** FIR filter with caller-provided Q15 coefficients and state (taps entries). */
typedef struct {
    const q15_t *coefs;
    q31_t *state;
    uint16_t taps;
    uint16_t pos;
} dsp_fir_t;

void dsp_fir_init(dsp_fir_t *f, const q15_t *coefs, uint16_t taps, q31_t *state);
void dsp_fir_process(dsp_fir_t *f, const q31_t *in, q31_t *out, uint32_t n);

/** DC blocker: y[n] = x[n] - x[n-1] + R * y[n-1]. */
typedef struct {
    q31_t r;
    q31_t x1;
    q31_t y1;
} dsp_dcblock_t;

void dsp_dcblock_init(dsp_dcblock_t *d, float fs, float fc);
void dsp_dcblock_process(dsp_dcblock_t *d, q31_t *buf, uint32_t n);

/** Attack/release envelope follower (operates on any non-negative int32 scale). */
typedef struct {
    q31_t att;
    q31_t rel;
    int32_t y;
} dsp_env_t;

void dsp_env_config(dsp_env_t *e, float attack_ms, float release_ms, float rate_hz);
int32_t dsp_env_update(dsp_env_t *e, int32_t x);

/* -------------------------------------------------------------------------- */
/*  Spectral analysis                                                         */
/* -------------------------------------------------------------------------- */

/** Build twiddle/log tables. Must be called once before FFT/log functions. */
void dsp_init(void);

/** In-place radix-2 complex FFT, Q15, scaled by 1/N. @p log2n <= DSP_FFT_MAX_LOG2. */
void dsp_fft_q15(cq15_t *x, uint32_t log2n);

/** Fill a Hann window of length n (Q15). */
void dsp_window_hann_q15(q15_t *w, uint32_t n);

/** |x|^2 of a complex Q15 value. */
static inline uint32_t dsp_cmag_sq(cq15_t v)
{
    return (uint32_t)((int32_t)v.re * v.re) + (uint32_t)((int32_t)v.im * v.im);
}

/**
 * Autocorrelation r[lag] = sum x[i] * x[i - lag] for lag in [lag_min, lag_max].
 * @p out must hold (lag_max - lag_min + 1) entries (scaled >> 8 to fit int32).
 */
void dsp_autocorr_q15(const int16_t *x, uint32_t n, uint32_t lag_min, uint32_t lag_max, int32_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DSP_H */
