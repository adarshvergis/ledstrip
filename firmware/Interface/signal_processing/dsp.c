/**
 * @file dsp.c
 * @brief Fixed-point DSP primitives: gains, envelopes, filters, logs, utilities.
 */
#include "dsp.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* log2(1 + i/32) in Q16, i = 0..32 */
static const uint16_t s_log2_lut[33] = {
    0, 2909, 5732, 8473, 11136, 13727, 16248, 18704, 21098, 23433, 25711, 27936, 30109,
    32234, 34312, 36346, 38336, 40286, 42196, 44068, 45904, 47705, 49472, 51207, 52911,
    54584, 56229, 57845, 59434, 60997, 62534, 64047, 65535,
};

/* -------------------------------------------------------------------------- */
/*  Conversions (design time)                                                 */
/* -------------------------------------------------------------------------- */

q31_t dsp_float_to_q31(float v)
{
    if (v >= 1.0f) return Q31_ONE;
    if (v <= -1.0f) return INT32_MIN;
    return (q31_t)(v * 2147483648.0f);
}

q15_t dsp_float_to_q15(float v)
{
    if (v >= 1.0f) return Q15_ONE;
    if (v <= -1.0f) return INT16_MIN;
    return (q15_t)(v * 32768.0f);
}

int32_t dsp_gain_db_to_q16(float gain_db)
{
    float g = powf(10.0f, gain_db / 20.0f);
    if (g > 32767.0f) g = 32767.0f;
    return (int32_t)(g * 65536.0f);
}

q31_t dsp_tau_to_alpha(float tau_ms, float rate_hz)
{
    if (tau_ms <= 0.0f || rate_hz <= 0.0f) {
        return Q31_ONE;
    }
    float a = 1.0f - expf(-1000.0f / (tau_ms * rate_hz));
    return dsp_float_to_q31(a);
}

/* -------------------------------------------------------------------------- */
/*  Block utilities                                                           */
/* -------------------------------------------------------------------------- */

void dsp_gain(q31_t *buf, uint32_t n, int32_t gain_q16)
{
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = dsp_sat_q31(((int64_t)buf[i] * gain_q16) >> 16);
    }
}

uint32_t dsp_peak_abs(const q31_t *buf, uint32_t n)
{
    uint32_t pk = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = dsp_abs_q31(buf[i]);
        if (a > pk) pk = a;
    }
    return pk;
}

uint32_t dsp_rms(const q31_t *buf, uint32_t n)
{
    if (n == 0) return 0;
    uint64_t acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t s = buf[i] >> 8;
        acc += (uint64_t)((int64_t)s * s);
    }
    return dsp_isqrt64(acc / n) << 8;
}

uint64_t dsp_energy(const q31_t *buf, uint32_t n)
{
    uint64_t acc = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t s = buf[i] >> 16;
        acc += (uint64_t)((int32_t)s * s);
    }
    return acc;
}

void dsp_baseline_remove(q31_t *buf, uint32_t n)
{
    if (n < 2) return;
    int64_t sy = 0, sxy = 0;
    for (uint32_t i = 0; i < n; i++) {
        sy += buf[i];
        sxy += (int64_t)i * buf[i];
    }
    /* least squares line y = a + b*i (coefficients in soft float, applied in fixed point) */
    float fn = (float)n;
    float sx = fn * (fn - 1.0f) * 0.5f;
    float sxx = (fn - 1.0f) * fn * (2.0f * fn - 1.0f) / 6.0f;
    float den = fn * sxx - sx * sx;
    float b = (fn * (float)sxy - sx * (float)sy) / den;
    float a = ((float)sy - b * sx) / fn;
    int64_t a_q16 = (int64_t)(a * 65536.0f);
    int64_t b_q16 = (int64_t)(b * 65536.0f);
    for (uint32_t i = 0; i < n; i++) {
        int64_t trend = (a_q16 + b_q16 * (int64_t)i) >> 16;
        buf[i] = dsp_sat_q31((int64_t)buf[i] - trend);
    }
}

void dsp_interp_linear(const q31_t *in, uint32_t n_in, q31_t *out, uint32_t n_out)
{
    if (n_in == 0 || n_out == 0) return;
    if (n_in == 1 || n_out == 1) {
        for (uint32_t j = 0; j < n_out; j++) out[j] = in[0];
        return;
    }
    /* position step in Q16 */
    uint32_t step = (uint32_t)((((uint64_t)(n_in - 1)) << 16) / (n_out - 1));
    uint32_t pos = 0;
    for (uint32_t j = 0; j < n_out; j++, pos += step) {
        uint32_t i = pos >> 16;
        uint32_t f = pos & 0xFFFF;
        if (i >= n_in - 1) {
            out[j] = in[n_in - 1];
        } else {
            int64_t d = (int64_t)in[i + 1] - in[i];
            out[j] = (q31_t)(in[i] + ((d * f) >> 16));
        }
    }
}

uint16_t dsp_rand16(uint32_t *state)
{
    uint32_t x = *state ? *state : 0x12345678u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return (uint16_t)(x >> 8);
}

/* -------------------------------------------------------------------------- */
/*  Logarithms / roots                                                        */
/* -------------------------------------------------------------------------- */

uint32_t dsp_isqrt64(uint64_t x)
{
    uint64_t res = 0;
    uint64_t bit = 1ull << 62;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)res;
}

int32_t dsp_log2_q16(uint64_t x)
{
    if (x == 0) return INT32_MIN;
    int msb = 63 - __builtin_clzll(x);
    uint32_t m;                                 /* mantissa with MSB at bit 31 */
    if (msb >= 31) {
        m = (uint32_t)(x >> (msb - 31));
    } else {
        m = (uint32_t)(x << (31 - msb));
    }
    uint32_t frac = m & 0x7FFFFFFFu;            /* [0,1) in Q31 */
    uint32_t idx = frac >> 26;                  /* 5 bits */
    uint32_t rem = (frac >> 10) & 0xFFFF;       /* interpolation weight Q16 */
    uint32_t lo = s_log2_lut[idx];
    uint32_t hi = s_log2_lut[idx + 1];
    uint32_t f = lo + (((hi - lo) * rem) >> 16);
    return (msb << 16) + (int32_t)f;
}

int32_t dsp_power_to_db_q8(uint64_t p)
{
    if (p == 0) return -200 * 256;
    return (int32_t)(((int64_t)dsp_log2_q16(p) * 771) >> 16);    /* 10*log10(2)*256 */
}

int32_t dsp_amp_to_db_q8(uint64_t a)
{
    if (a == 0) return -200 * 256;
    return (int32_t)(((int64_t)dsp_log2_q16(a) * 1541) >> 16);   /* 20*log10(2)*256 */
}

/* 2^(i/32) in Q16, i = 0..32 */
static const uint32_t s_exp2_lut[33] = {
    65536, 66971, 68438, 69936, 71468, 73032, 74632, 76266, 77936, 79642, 81386, 83169,
    84990, 86851, 88752, 90696, 92682, 94711, 96785, 98905, 101070, 103283, 105545, 107856,
    110218, 112631, 115098, 117618, 120194, 122825, 125515, 128263, 131072,
};

uint32_t dsp_exp2_q16(int32_t x_q16)
{
    if (x_q16 >= (15 << 16)) return UINT32_MAX;
    int32_t ip = x_q16 >> 16;                   /* floor (arithmetic shift) */
    uint32_t f = (uint32_t)x_q16 & 0xFFFFu;
    uint32_t idx = f >> 11;                     /* 5 bits */
    uint32_t rem = (f & 0x7FFu) << 5;           /* Q16 weight */
    uint32_t lo = s_exp2_lut[idx], hi = s_exp2_lut[idx + 1];
    uint32_t m = lo + (uint32_t)(((uint64_t)(hi - lo) * rem) >> 16);   /* [1,2) in Q16 */
    if (ip >= 0) return m << ip;
    if (ip <= -31) return 0;
    return m >> (-ip);
}

uint32_t dsp_db_q8_to_lin_q16(int32_t db_q8)
{
    /* 10^(dB/20) = 2^(dB / 6.0206); 65536 / (6.0206 * 256) * 256 = 10885 */
    int32_t x = (int32_t)(((int64_t)db_q8 * 10885) >> 8);
    return dsp_exp2_q16(x);
}

/* -------------------------------------------------------------------------- */
/*  Biquad                                                                    */
/* -------------------------------------------------------------------------- */

static int32_t coef_q30(double v)
{
    if (v >= 2.0) return INT32_MAX;
    if (v <= -2.0) return INT32_MIN;
    return (int32_t)lround(v * 1073741824.0);
}

void dsp_biquad_design(dsp_biquad_coef_t *c, dsp_biquad_type_t type, float fs, float f0, float q, float gain_db)
{
    memset(c, 0, sizeof(*c));
    if (type == DSP_BQ_BYPASS || fs <= 0.0f || f0 <= 0.0f || f0 >= fs * 0.5f || q <= 0.0f) {
        c->b0 = coef_q30(1.0);
        c->bypass = true;
        return;
    }
    /* Robert Bristow-Johnson "Audio EQ Cookbook" */
    double A = pow(10.0, gain_db / 40.0);
    double w0 = 2.0 * M_PI * f0 / fs;
    double cw = cos(w0), sw = sin(w0);
    double alpha = sw / (2.0 * q);
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

    switch (type) {
    case DSP_BQ_LOWPASS:
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case DSP_BQ_HIGHPASS:
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case DSP_BQ_BANDPASS:
        b0 = alpha; b1 = 0; b2 = -alpha;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case DSP_BQ_NOTCH:
        b0 = 1; b1 = -2 * cw; b2 = 1;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case DSP_BQ_PEAK:
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
        break;
    case DSP_BQ_LOWSHELF: {
        double s = 2 * sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + s);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - s);
        a0 = (A + 1) + (A - 1) * cw + s;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - s;
        break;
    }
    case DSP_BQ_HIGHSHELF: {
        double s = 2 * sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + s);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - s);
        a0 = (A + 1) - (A - 1) * cw + s;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - s;
        break;
    }
    default:
        break;
    }
    c->b0 = coef_q30(b0 / a0);
    c->b1 = coef_q30(b1 / a0);
    c->b2 = coef_q30(b2 / a0);
    c->a1 = coef_q30(a1 / a0);
    c->a2 = coef_q30(a2 / a0);
    c->bypass = false;
}

void dsp_biquad_set_coefs(dsp_biquad_t *bq, const int32_t coefs_q30[5])
{
    bq->c.b0 = coefs_q30[0];
    bq->c.b1 = coefs_q30[1];
    bq->c.b2 = coefs_q30[2];
    bq->c.a1 = coefs_q30[3];
    bq->c.a2 = coefs_q30[4];
    bq->c.bypass = false;
}

void dsp_biquad_reset(dsp_biquad_t *bq)
{
    bq->x1 = bq->x2 = bq->y1 = bq->y2 = 0;
}

void dsp_biquad_process(dsp_biquad_t *bq, q31_t *buf, uint32_t n)
{
    if (bq->c.bypass) return;
    const int32_t b0 = bq->c.b0, b1 = bq->c.b1, b2 = bq->c.b2, a1 = bq->c.a1, a2 = bq->c.a2;
    int32_t x1 = bq->x1, x2 = bq->x2, y1 = bq->y1, y2 = bq->y2;
    for (uint32_t i = 0; i < n; i++) {
        int32_t x0 = buf[i];
        /* products >> 2 give 2 bits of accumulator headroom (Q2.30 * Q31 -> Q61) */
        int64_t acc = ((int64_t)b0 * x0 >> 2) + ((int64_t)b1 * x1 >> 2) + ((int64_t)b2 * x2 >> 2)
                    - ((int64_t)a1 * y1 >> 2) - ((int64_t)a2 * y2 >> 2);
        int32_t y0 = dsp_sat_q31(acc >> 28);
        x2 = x1; x1 = x0;
        y2 = y1; y1 = y0;
        buf[i] = y0;
    }
    bq->x1 = x1; bq->x2 = x2; bq->y1 = y1; bq->y2 = y2;
}

/* -------------------------------------------------------------------------- */
/*  FIR                                                                       */
/* -------------------------------------------------------------------------- */

void dsp_fir_init(dsp_fir_t *f, const q15_t *coefs, uint16_t taps, q31_t *state)
{
    f->coefs = coefs;
    f->taps = taps;
    f->state = state;
    f->pos = 0;
    memset(state, 0, taps * sizeof(q31_t));
}

void dsp_fir_process(dsp_fir_t *f, const q31_t *in, q31_t *out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        f->state[f->pos] = in[i];
        int64_t acc = 0;
        uint32_t idx = f->pos;
        for (uint32_t k = 0; k < f->taps; k++) {
            acc += (int64_t)f->coefs[k] * f->state[idx];
            idx = (idx == 0) ? f->taps - 1u : idx - 1u;
        }
        out[i] = dsp_sat_q31(acc >> 15);
        f->pos = (f->pos + 1u) % f->taps;
    }
}

/* -------------------------------------------------------------------------- */
/*  DC blocker / envelope                                                     */
/* -------------------------------------------------------------------------- */

void dsp_dcblock_init(dsp_dcblock_t *d, float fs, float fc)
{
    float r = 1.0f - (2.0f * (float)M_PI * fc / fs);
    if (r < 0.5f) r = 0.5f;
    d->r = dsp_float_to_q31(r);
    d->x1 = 0;
    d->y1 = 0;
}

void dsp_dcblock_process(dsp_dcblock_t *d, q31_t *buf, uint32_t n)
{
    q31_t x1 = d->x1, y1 = d->y1;
    const q31_t r = d->r;
    for (uint32_t i = 0; i < n; i++) {
        q31_t x0 = buf[i];
        int64_t y = (int64_t)x0 - x1 + (((int64_t)r * y1) >> 31);
        y1 = dsp_sat_q31(y);
        x1 = x0;
        buf[i] = y1;
    }
    d->x1 = x1;
    d->y1 = y1;
}

void dsp_env_config(dsp_env_t *e, float attack_ms, float release_ms, float rate_hz)
{
    e->att = dsp_tau_to_alpha(attack_ms, rate_hz);
    e->rel = dsp_tau_to_alpha(release_ms, rate_hz);
}

int32_t dsp_env_update(dsp_env_t *e, int32_t x)
{
    e->y = dsp_smooth(e->y, x, (x > e->y) ? e->att : e->rel);
    return e->y;
}

/* -------------------------------------------------------------------------- */
/*  Autocorrelation                                                           */
/* -------------------------------------------------------------------------- */

void dsp_autocorr_q15(const int16_t *x, uint32_t n, uint32_t lag_min, uint32_t lag_max, int32_t *out)
{
    for (uint32_t lag = lag_min; lag <= lag_max; lag++) {
        int64_t acc = 0;
        for (uint32_t i = lag; i < n; i++) {
            acc += (int32_t)x[i] * x[i - lag];
        }
        acc >>= 12;
        out[lag - lag_min] = (acc > INT32_MAX) ? INT32_MAX : (acc < INT32_MIN ? INT32_MIN : (int32_t)acc);
    }
}
