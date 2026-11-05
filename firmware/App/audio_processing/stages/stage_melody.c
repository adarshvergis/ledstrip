/**
 * @file stage_melody.c
 * @brief Lead voice tracker: finds the prominent pitched voice (solo guitar, vocal,
 *        synth lead), its pitch and its note starts.
 *
 * Every 2nd block (8 ms) the last 512 samples are Hann windowed and transformed
 * (31.25 Hz bins, block floating point as in stage_spectrum.c).
 * 1. Harmonic sum: for every candidate fundamental bin k in [min_freq, max_freq]
 *    H(k) = L(k) + L(2k)/2 + L(3k)/3, with L = power of the 3-bin main lobe.
 * 2. Salience = H(best) / power of everything above min_freq: a single dominant
 *    voice gives ~0.5..1, chords ~0.2, drums / noise ~0.05.
 * 3. Pitch: parabolic interpolation (in dB) around the fundamental bin, as semitones.
 * 4. Notes: a voice is active while the salience stays above the threshold; a new
 *    note starts on a pitch change of > 0.8 semitone that holds for note_min_ms, or on
 *    a re-attack (sudden energy jump at the same pitch). Small pitch changes (bends,
 *    vibrato) glide instead of starting notes.
 */
#include "audio_types.h"

#include <math.h>
#include <string.h>

#define N           512u
#define LOG2N       9u
#define NH          (N / 2u)
#define BIN_HZ      ((float)SAMPLE_RATE / (float)N)
#define FRAME_HZ    (BLOCK_RATE_HZ / 2.0f)
#define SEMI_Q8     256
#define SAME_Q8     154             /* 0.6 semitone: same note (vibrato, bends)  */
#define JUMP_Q8     205             /* 0.8 semitone: a different note            */
#define LEVEL_RANGE (30 * 256)      /* dB Q8 mapped to the 0..1 voice level      */

_Static_assert(LOG2N <= DSP_FFT_MAX_LOG2, "FFT too large for the DSP library");

typedef struct {
    int16_t  ring[N];
    uint16_t pos;
    cq15_t   buf[N];
    q15_t    win[N];
    uint32_t pw[NH];
    uint64_t lobe[NH];
    uint16_t kmin, kmax;
    int32_t  log2_ref;              /* log2(bin of A4 in Q8 bins), Q16 */
    int32_t  sal_thr;               /* Q15 */
    uint8_t  min_frames;
    int32_t  reattack;              /* dB Q8 */
    q31_t    a_sal, a_glide, a_slow;
    /* runtime */
    bool     odd;
    int32_t  sal;                   /* smoothed salience, Q15 */
    int32_t  cand;                  /* candidate pitch, Q8 semitones */
    uint8_t  stable;
    uint8_t  quiet;
    int32_t  e_slow, peak;          /* dB Q8 */
    bool     armed;
    melody_t out;
} melody_state_t;

static melody_state_t s_st;

static const param_desc_t s_params[MELODY_P_COUNT] = {
    [MELODY_P_MIN_FREQ_HZ] = PDESC_F("min_freq", "Hz", 80.0f, 1000.0f, 150.0f, 5.0f),
    [MELODY_P_MAX_FREQ_HZ] = PDESC_F("max_freq", "Hz", 400.0f, 4000.0f, 2000.0f, 10.0f),
    [MELODY_P_SALIENCE]    = PDESC_F("salience", "", 0.05f, 0.9f, 0.35f, 0.01f),
    [MELODY_P_NOTE_MIN_MS] = PDESC_I("note_min", "ms", 8.0f, 200.0f, 24.0f),
    [MELODY_P_GLIDE_MS]    = PDESC_I("glide", "ms", 0.0f, 200.0f, 20.0f),
    [MELODY_P_REATTACK_DB] = PDESC_F("reattack", "dB", 1.0f, 15.0f, 4.0f, 0.5f),
};

static void reset(void *st)
{
    melody_state_t *s = st;
    memset(s->ring, 0, sizeof(s->ring));
    s->pos = 0;
    s->odd = false;
    s->sal = 0;
    s->cand = 0;
    s->stable = 0;
    s->quiet = 0;
    s->e_slow = -100 * 256;
    s->peak = -100 * 256;
    s->armed = true;
    memset(&s->out, 0, sizeof(s->out));
}

static void configure(void *st, const float *p)
{
    melody_state_t *s = st;
    dsp_window_hann_q15(s->win, N);
    int kmin = (int)lroundf(p[MELODY_P_MIN_FREQ_HZ] / BIN_HZ);
    int kmax = (int)lroundf(p[MELODY_P_MAX_FREQ_HZ] / BIN_HZ);
    if (kmin < 2) kmin = 2;
    if (kmax > (int)NH - 2) kmax = (int)NH - 2;
    if (kmax < kmin + 2) kmax = kmin + 2;
    s->kmin = (uint16_t)kmin;
    s->kmax = (uint16_t)kmax;
    s->log2_ref = (int32_t)(log2f(440.0f / BIN_HZ * 256.0f) * 65536.0f);
    s->sal_thr = dsp_float_to_q15(p[MELODY_P_SALIENCE]);
    float frames = ceilf(p[MELODY_P_NOTE_MIN_MS] / (2.0f * BLOCK_MS));
    s->min_frames = (uint8_t)(frames < 1.0f ? 1.0f : frames);
    s->reattack = (int32_t)(p[MELODY_P_REATTACK_DB] * 256.0f);
    s->a_sal = dsp_tau_to_alpha(25.0f, FRAME_HZ);
    s->a_glide = p[MELODY_P_GLIDE_MS] < 1.0f ? Q31_ONE : dsp_tau_to_alpha(p[MELODY_P_GLIDE_MS], FRAME_HZ);
    s->a_slow = dsp_tau_to_alpha(80.0f, FRAME_HZ);
}

static int32_t iabs(int32_t v)
{
    return v < 0 ? -v : v;
}

static uint64_t harmonic_sum(const melody_state_t *s, uint32_t k)
{
    uint64_t h = s->lobe[k];
    if (2 * k < NH - 1) h += s->lobe[2 * k] / 2;
    if (3 * k < NH - 1) h += s->lobe[3 * k] / 3;
    return h;
}

/* k is f0's own lobe or lies on a harmonic / subharmonic of f0 (a chord tone two
 * bins away is a different voice) */
static bool related(uint32_t k, uint32_t f0)
{
    if (iabs((int32_t)k - (int32_t)f0) <= 1) return true;
    for (uint32_t m = 2; m <= 3; m++) {
        if (iabs((int32_t)k - (int32_t)(f0 * m)) <= 2 || iabs((int32_t)(k * m) - (int32_t)f0) <= 2) return true;
    }
    return false;
}

/* One FFT frame: salience, pitch, note tracking. */
static void analyse(melody_state_t *s)
{
    int32_t mx = 1;
    for (uint32_t i = 0; i < N; i++) {
        int32_t a = s->ring[i] < 0 ? -s->ring[i] : s->ring[i];
        if (a > mx) mx = a;
    }
    int shift = __builtin_clz((uint32_t)mx) - 17;
    if (shift < 0) shift = 0;
    if (shift > 12) shift = 12;
    for (uint32_t i = 0; i < N; i++) {
        int32_t x = (int32_t)s->ring[(s->pos + i) & (N - 1u)] << shift;
        s->buf[i].re = (int16_t)((x * s->win[i]) >> 15);
        s->buf[i].im = 0;
    }
    dsp_fft_q15(s->buf, LOG2N);

    for (uint32_t k = 0; k < NH; k++) s->pw[k] = dsp_cmag_sq(s->buf[k]);
    s->lobe[0] = s->lobe[NH - 1] = 0;
    for (uint32_t k = 1; k < NH - 1; k++) s->lobe[k] = (uint64_t)s->pw[k - 1] + s->pw[k] + s->pw[k + 1];
    uint64_t total = 0;
    for (uint32_t k = s->kmin - 1u; k < NH; k++) total += s->pw[k];

    /* candidates must have a real fundamental: a spectral peak carrying >= 1/4 of the
     * harmonic sum (a chord's "missing fundamental" or a bass leaking in does not count) */
    uint64_t best_h = 0, second_h = 0;
    uint32_t best = s->kmin;
    for (uint32_t k = s->kmin; k <= s->kmax; k++) {
        if (s->pw[k] < s->pw[k - 1] || s->pw[k] < s->pw[k + 1]) continue;
        uint64_t h = harmonic_sum(s, k);
        if (s->lobe[k] * 4 < h) continue;
        if (h > best_h) {
            best_h = h;
            best = k;
        }
    }
    /* strongest voice that is not part of the winner's harmonic series */
    for (uint32_t k = s->kmin; k <= s->kmax; k++) {
        if (s->pw[k] < s->pw[k - 1] || s->pw[k] < s->pw[k + 1] || related(k, best)) continue;
        uint64_t h = harmonic_sum(s, k);
        if (s->lobe[k] * 4 >= h && h > second_h) second_h = h;
    }
    /* salience = share of the voice in the total x dominance over the next voice */
    int32_t raw = 0;
    if (total && best_h) {
        uint64_t frac = (best_h * 32767u) / total;
        uint64_t dom = ((best_h - (second_h < best_h ? second_h : best_h)) * 2u * 32767u) / best_h;
        if (frac > 32767) frac = 32767;
        if (dom > 32767) dom = 32767;
        raw = (int32_t)((frac * dom) >> 15);
    }
    s->sal = dsp_smooth(s->sal, raw, s->a_sal);

    /* loudness of the voice region, relative to its own decaying peak */
    int32_t e_db = dsp_power_to_db_q8(total ? total : 1) - 1541 * shift;
    if (e_db > s->peak) s->peak = e_db;
    else s->peak -= (int32_t)(3.0f * 256.0f / FRAME_HZ);
    int32_t lvl = ((e_db - (s->peak - LEVEL_RANGE)) * 32767) / LEVEL_RANGE;
    s->out.level = (uint16_t)(lvl < 0 ? 0 : (lvl > 32767 ? 32767 : lvl));
    int32_t jump = e_db - s->e_slow;
    s->e_slow = dsp_smooth(s->e_slow, e_db, s->a_slow);

    /* pitch: strongest bin of the fundamental's lobe, parabolic interpolation in dB */
    uint32_t kp = best;
    if (s->pw[best - 1] > s->pw[kp]) kp = best - 1;
    if (s->pw[best + 1] > s->pw[kp]) kp = best + 1;
    int32_t a = dsp_power_to_db_q8(s->pw[kp - 1] + 1u);
    int32_t b = dsp_power_to_db_q8(s->pw[kp] + 1u);
    int32_t c = dsp_power_to_db_q8(s->pw[kp + 1] + 1u);
    int32_t den = a - 2 * b + c;
    int32_t d_q8 = den < 0 ? ((a - c) * 128) / den : 0;
    if (d_q8 > 128) d_q8 = 128;
    if (d_q8 < -128) d_q8 = -128;
    int32_t bin_q8 = (int32_t)kp * 256 + d_q8;
    int32_t pitch = 69 * SEMI_Q8 + (int32_t)(((int64_t)12 * (dsp_log2_q16((uint64_t)bin_q8) - s->log2_ref)) >> 8);

    /* note tracking */
    if (iabs(pitch - s->cand) < SAME_Q8) {
        s->cand += (pitch - s->cand) / 2;
        if (s->stable < 255) s->stable++;
    } else {
        s->cand = pitch;
        s->stable = 1;
    }
    bool was_active = s->out.active;
    if (s->sal > s->sal_thr) {
        s->quiet = 0;
    } else if (s->sal < (s->sal_thr * 4) / 5 && s->quiet < 255) {
        s->quiet++;
    }
    if (!was_active) {
        if (s->sal > s->sal_thr && s->stable >= s->min_frames) {
            s->out.active = true;
            s->out.note_on = true;
            s->out.pitch_q8 = s->cand;
        }
    } else if (s->quiet >= 2) {
        s->out.active = false;
    } else if (s->stable >= s->min_frames && iabs(s->cand - s->out.pitch_q8) > JUMP_Q8) {
        s->out.note_on = true;                                  /* new note */
        s->out.pitch_q8 = s->cand;
    } else {
        if (s->stable >= 2) s->out.pitch_q8 = dsp_smooth(s->out.pitch_q8, s->cand, s->a_glide);
        if (s->armed && jump > s->reattack && s->stable >= 2) s->out.note_on = true;   /* re-attack */
    }
    if (jump > s->reattack) s->armed = false;
    else if (jump < s->reattack / 3) s->armed = true;
    s->out.salience = (uint16_t)(s->sal < 0 ? 0 : s->sal);
}

static void process(void *st, audio_frame_t *f)
{
    melody_state_t *s = st;
    for (uint16_t i = 0; i < f->n; i++) {
        s->ring[s->pos] = (int16_t)(f->samples[i] >> 16);
        s->pos = (uint16_t)((s->pos + 1u) & (N - 1u));
    }
    s->odd = !s->odd;
    if (s->odd) analyse(s);
    f->melody = s->out;
    s->out.note_on = false;
}

const stage_def_t g_stage_melody = {
    .id = STAGE_MELODY,
    .name = "Melody",
    .desc = "Lead voice tracker: 512-pt FFT, harmonic-sum pitch, note starts",
    .params = s_params,
    .num_params = MELODY_P_COUNT,
    .state = &s_st,
    .reset = reset,
    .configure = configure,
    .process = process,
};
