/**
 * @file dsp_fft.c
 * @brief Radix-2 decimation-in-time complex FFT in Q15 with per-stage scaling.
 *
 * Every butterfly stage halves its outputs, so the result is X[k]/N and can never
 * overflow. Twiddles are generated once by dsp_init() for the maximum size and
 * shared by all smaller transforms.
 */
#include "dsp.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int16_t s_cos[DSP_FFT_MAX_SIZE / 2];
static int16_t s_sin[DSP_FFT_MAX_SIZE / 2];
static bool    s_ready;

void dsp_init(void)
{
    if (s_ready) return;
    for (uint32_t i = 0; i < DSP_FFT_MAX_SIZE / 2; i++) {
        double a = 2.0 * M_PI * (double)i / (double)DSP_FFT_MAX_SIZE;
        s_cos[i] = dsp_float_to_q15((float)cos(a));
        s_sin[i] = dsp_float_to_q15((float)sin(a));
    }
    s_ready = true;
}

void dsp_window_hann_q15(q15_t *w, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        double v = 0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)n);
        w[i] = dsp_float_to_q15((float)v);
    }
}

void dsp_fft_q15(cq15_t *x, uint32_t log2n)
{
    if (!s_ready || log2n == 0 || log2n > DSP_FFT_MAX_LOG2) return;
    const uint32_t n = 1u << log2n;

    /* bit-reversal permutation */
    for (uint32_t i = 1, j = 0; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j |= bit;
        if (i < j) {
            cq15_t t = x[i];
            x[i] = x[j];
            x[j] = t;
        }
    }

    for (uint32_t len = 2; len <= n; len <<= 1) {
        const uint32_t half = len >> 1;
        const uint32_t tstep = DSP_FFT_MAX_SIZE / len;
        for (uint32_t k = 0; k < n; k += len) {
            uint32_t ti = 0;
            for (uint32_t j = 0; j < half; j++, ti += tstep) {
                /* W = cos - j sin  (forward transform) */
                int32_t wr = s_cos[ti];
                int32_t wi = -s_sin[ti];
                cq15_t *a = &x[k + j];
                cq15_t *b = &x[k + j + half];
                int32_t tr = (wr * b->re - wi * b->im) >> 15;
                int32_t tim = (wr * b->im + wi * b->re) >> 15;
                int32_t ar = a->re, ai = a->im;
                a->re = (int16_t)((ar + tr) >> 1);
                a->im = (int16_t)((ai + tim) >> 1);
                b->re = (int16_t)((ar - tr) >> 1);
                b->im = (int16_t)((ai - tim) >> 1);
            }
        }
    }
}
