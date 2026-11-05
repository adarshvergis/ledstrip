/**
 * @file stage_spectrum.c
 * @brief Sliding FFT spectrum analyser: log-spaced bands, spectral centroid.
 *
 * Every block the last AUDIO_FFT_SIZE samples are Hann windowed and transformed
 * (75 % overlap at 256/64). Block floating point: the window is normalised to the
 * full Q15 range before the FFT and the shift is compensated in dB, so quiet signals
 * keep their resolution. Band power in dBFS is mapped to 0..1 either against a fixed
 * floor or against a per-band decaying peak (normalise = on).
 */
#include "audio_types.h"

#include <math.h>
#include <string.h>

#define N           AUDIO_FFT_SIZE
#define NB          ((int)AUDIO_NUM_BANDS)
#define FS_REF_Q8   (80 * 256)      /* power of a full-scale sine after window+FFT, dB */
#define DB16(x)     ((int32_t)((x) * 65536.0f))

typedef struct {
    int16_t  ring[N];
    uint16_t pos;
    cq15_t   buf[N];
    q15_t    win[N];
    uint16_t edge[NB + 1];          /* FFT bin edges of the bands */
    int32_t  peak[NB];              /* dB Q16, per band normaliser */
    int32_t  smooth[NB];            /* Q15 */
    int32_t  range;                 /* dB Q16 */
    int32_t  floor_db;              /* dB Q16 */
    int32_t  decay;                 /* dB Q16 per block */
    q31_t    att, rel;
    bool     normalize;
} spectrum_state_t;

static spectrum_state_t s_st;

static const param_desc_t s_params[SPECTRUM_P_COUNT] = {
    [SPECTRUM_P_MIN_FREQ_HZ]    = PDESC_F("min_freq", "Hz", 30.0f, 500.0f, 60.0f, 1.0f),
    [SPECTRUM_P_MAX_FREQ_HZ]    = PDESC_F("max_freq", "Hz", 1000.0f, 7900.0f, 6000.0f, 10.0f),
    [SPECTRUM_P_RANGE_DB]       = PDESC_F("range", "dB", 10.0f, 80.0f, 36.0f, 1.0f),
    [SPECTRUM_P_FLOOR_DB]       = PDESC_F("floor", "dBFS", -110.0f, -30.0f, -80.0f, 1.0f),
    [SPECTRUM_P_ATTACK_MS]      = PDESC_F("attack", "ms", 0.0f, 200.0f, 8.0f, 1.0f),
    [SPECTRUM_P_RELEASE_MS]     = PDESC_F("release", "ms", 10.0f, 2000.0f, 180.0f, 5.0f),
    [SPECTRUM_P_NORMALIZE]      = PDESC_B("normalize", 1.0f),
    [SPECTRUM_P_NORM_DECAY_DB_S]= PDESC_F("norm_decay", "dB/s", 0.2f, 20.0f, 2.0f, 0.1f),
};

static void reset(void *st)
{
    spectrum_state_t *s = st;
    memset(s->ring, 0, sizeof(s->ring));
    s->pos = 0;
    for (int b = 0; b < NB; b++) {
        s->peak[b] = DB16(-50.0f);
        s->smooth[b] = 0;
    }
}

static void configure(void *st, const float *p)
{
    spectrum_state_t *s = st;
    dsp_window_hann_q15(s->win, N);

    float fmin = p[SPECTRUM_P_MIN_FREQ_HZ];
    float fmax = p[SPECTRUM_P_MAX_FREQ_HZ];
    if (fmax > SAMPLE_RATE * 0.5f) fmax = SAMPLE_RATE * 0.5f;
    if (fmax < fmin * 2.0f) fmax = fmin * 2.0f;
    const float bin_hz = (float)SAMPLE_RATE / N;
    int prev = -1;
    for (int b = 0; b <= NB; b++) {
        float f = fmin * powf(fmax / fmin, (float)b / NB);
        int k = (int)lroundf(f / bin_hz);
        if (k < 1) k = 1;
        if (k <= prev) k = prev + 1;            /* every band gets >= 1 bin */
        if (k > (int)(N / 2)) k = (int)(N / 2);
        s->edge[b] = (uint16_t)k;
        prev = k;
    }
    s->range = DB16(p[SPECTRUM_P_RANGE_DB]);
    s->floor_db = DB16(p[SPECTRUM_P_FLOOR_DB]);
    s->decay = DB16(p[SPECTRUM_P_NORM_DECAY_DB_S] / BLOCK_RATE_HZ);
    s->att = dsp_tau_to_alpha(p[SPECTRUM_P_ATTACK_MS], BLOCK_RATE_HZ);
    s->rel = dsp_tau_to_alpha(p[SPECTRUM_P_RELEASE_MS], BLOCK_RATE_HZ);
    s->normalize = p[SPECTRUM_P_NORMALIZE] > 0.5f;
}

static void process(void *st, audio_frame_t *f)
{
    spectrum_state_t *s = st;

    /* append new samples (Q31 -> Q15) */
    for (uint16_t i = 0; i < f->n; i++) {
        s->ring[s->pos] = (int16_t)(f->samples[i] >> 16);
        s->pos = (uint16_t)((s->pos + 1u) & (N - 1u));
    }

    /* block floating point: find headroom of the window content */
    int32_t mx = 1;
    for (uint32_t i = 0; i < N; i++) {
        int32_t a = s->ring[i] < 0 ? -s->ring[i] : s->ring[i];
        if (a > mx) mx = a;
    }
    int shift = __builtin_clz((uint32_t)mx) - 17;      /* bits until 0x4000 */
    if (shift < 0) shift = 0;
    if (shift > 12) shift = 12;

    for (uint32_t i = 0; i < N; i++) {
        int32_t x = (int32_t)s->ring[(s->pos + i) & (N - 1u)] << shift;
        s->buf[i].re = (int16_t)((x * s->win[i]) >> 15);
        s->buf[i].im = 0;
    }
    dsp_fft_q15(s->buf, AUDIO_FFT_LOG2);

    const int32_t comp_q8 = FS_REF_Q8 + 1541 * shift;   /* 6.02 dB per bit of shift */
    uint64_t c_num = 0, c_den = 0;
    int32_t best = -1;
    uint8_t best_b = 0;

    for (int b = 0; b < NB; b++) {
        uint64_t pw = 0;
        for (uint16_t k = s->edge[b]; k < s->edge[b + 1]; k++) {
            uint32_t m = dsp_cmag_sq(s->buf[k]);
            pw += m;
            c_num += (uint64_t)m * k;
        }
        c_den += pw;
        int32_t db = (dsp_power_to_db_q8(pw) - comp_q8) * 256;   /* dBFS Q16 */
        int32_t db_q8 = db >> 8;
        f->band_db[b] = (int16_t)(db_q8 < -32000 ? -32000 : db_q8);

        int32_t lo;
        if (s->normalize) {
            if (db > s->peak[b]) s->peak[b] = db;
            else s->peak[b] -= s->decay;
            int32_t min_peak = s->floor_db + s->range;
            if (s->peak[b] < min_peak) s->peak[b] = min_peak;
            lo = s->peak[b] - s->range;
        } else {
            lo = s->floor_db;
        }
        int64_t v = ((int64_t)(db - lo) * 32767) / s->range;
        if (v < 0) v = 0;
        if (v > 32767) v = 32767;
        s->smooth[b] = dsp_smooth(s->smooth[b], (int32_t)v, (v > s->smooth[b]) ? s->att : s->rel);
        f->bands[b] = (uint16_t)s->smooth[b];
        if (s->smooth[b] > best) {
            best = s->smooth[b];
            best_b = (uint8_t)b;
        }
    }
    /* centroid as fraction of Nyquist (bin N/2) */
    f->centroid = c_den ? (uint16_t)((c_num * (32767u * 2u / N)) / c_den) : 0;
    f->dominant_band = best_b;
    f->spectrum_valid = true;
}

const stage_def_t g_stage_spectrum = {
    .id = STAGE_SPECTRUM,
    .name = "Spectrum",
    .desc = "Sliding FFT -> logarithmic bands (0..1) and spectral centroid",
    .params = s_params,
    .num_params = SPECTRUM_P_COUNT,
    .state = &s_st,
    .reset = reset,
    .configure = configure,
    .process = process,
};
