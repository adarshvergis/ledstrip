/**
 * @file stage_beat.c
 * @brief Main-beat detector: bass onset detection + autocorrelation tempo estimate +
 *        phase locked beat prediction.
 *
 * 1. Onset detection function (ODF): bass band (biquad low-pass) block energy in dB;
 *    ODF = max(0, E_dB - slow average of E_dB).
 * 2. Onset: ODF crosses an adaptive threshold max(min_onset, sensitivity * mean ODF).
 * 3. Tempo: the ODF (decimated to block_rate/2) is kept for ~3 s; every 32 blocks its
 *    autocorrelation over the lags of [min_bpm, max_bpm], weighted by a prior centred
 *    on 120 BPM, gives the period (parabolic interpolation for sub-lag precision).
 * 4. Beat output: with tempo lock, onsets close to the predicted beat time re-sync the
 *    phase, off-beat onsets are ignored, and missing beats are predicted ("flywheel")
 *    for a few periods. Without lock (or before a tempo is known) every onset is a beat.
 */
#include "audio_types.h"

#include <math.h>
#include <string.h>

#define HIST            384             /* decimated ODF history (~3 s) */
#define MAX_LAGS        200
#define TEMPO_EVERY     32              /* blocks between tempo estimates */
#define E_WIN           4               /* bass energy window (blocks), ~16 ms */
#define Q8              256

typedef struct {
    dsp_biquad_t lp;
    q31_t   scratch[AUDIO_HOP_SIZE];
    /* onset detection */
    uint64_t e_hist[E_WIN];
    uint8_t e_pos;
    bool    primed;
    bool    armed;
    int32_t e_slow;                     /* dB Q8 */
    int32_t odf_mean;                   /* dB Q8 */
    int32_t odf_prev;
    q31_t   a_slow, a_mean;
    int32_t min_onset;
    int32_t sens_q8;
    /* tempo */
    int16_t hist[HIST];
    uint16_t hpos;
    int32_t dec_acc;
    uint8_t dec_n;
    int16_t lin[HIST];
    int32_t corr[MAX_LAGS];
    int16_t prior[MAX_LAGS];
    uint16_t lag_min, lag_max;
    uint32_t period_q8;                 /* beat period in blocks, Q8, 0 = unknown */
    uint32_t cand_q8;
    uint8_t  cand_cnt;
    uint16_t confidence;                /* Q15 */
    uint16_t tempo_cnt;
    /* phase / output */
    int32_t t_q8;                       /* blocks since last beat, Q8 */
    int32_t min_interval_q8;
    bool    lock;
    uint16_t tol_q15;
    uint8_t flywheel_max;
    uint8_t flywheel;
    bool    last_predicted;
    uint16_t last_strength;
    uint32_t beat_count;
    /* phase re-acquisition */
    int32_t s_in;                       /* avg strength of in-phase onsets, Q15 */
    int32_t s_off;                      /* avg strength of consistent off-phase onsets */
    int32_t off_ph_q8;                  /* their offset from the beat, Q8 blocks */
    uint8_t off_cnt;
} beat_state_t;

static beat_state_t s_st;

static const param_desc_t s_params[BEAT_P_COUNT] = {
    [BEAT_P_BASS_CUTOFF_HZ] = PDESC_F("bass_cutoff", "Hz", 60.0f, 400.0f, 150.0f, 5.0f),
    [BEAT_P_SENSITIVITY]    = PDESC_F("threshold_factor", "x", 1.0f, 4.0f, 1.6f, 0.05f),
    [BEAT_P_MIN_ONSET_DB]   = PDESC_F("min_onset", "dB", 0.5f, 15.0f, 2.5f, 0.1f),
    [BEAT_P_MIN_BPM]        = PDESC_I("min_bpm", "BPM", 40.0f, 140.0f, 75.0f),
    [BEAT_P_MAX_BPM]        = PDESC_I("max_bpm", "BPM", 100.0f, 220.0f, 180.0f),
    [BEAT_P_TEMPO_LOCK]     = PDESC_B("tempo_lock", 1.0f),
    [BEAT_P_LOCK_TOLERANCE] = PDESC_F("lock_tolerance", "period", 0.05f, 0.45f, 0.18f, 0.01f),
    [BEAT_P_FLYWHEEL_BEATS] = PDESC_I("flywheel_beats", "beats", 0.0f, 16.0f, 4.0f),
};

static void reset(void *st)
{
    beat_state_t *s = st;
    dsp_biquad_reset(&s->lp);
    memset(s->e_hist, 0, sizeof(s->e_hist));
    s->e_pos = 0;
    s->primed = false;
    s->armed = true;
    s->e_slow = -60 * Q8;
    s->odf_mean = 1 * Q8;
    s->odf_prev = 0;
    memset(s->hist, 0, sizeof(s->hist));
    s->hpos = 0;
    s->dec_acc = 0;
    s->dec_n = 0;
    s->period_q8 = 0;
    s->cand_cnt = 0;
    s->confidence = 0;
    s->tempo_cnt = 0;
    s->t_q8 = 0;
    s->flywheel = 0;
    s->last_predicted = false;
    s->last_strength = 0;
    s->s_in = 0;
    s->s_off = 0;
    s->off_ph_q8 = 0;
    s->off_cnt = 0;
}

static void configure(void *st, const float *p)
{
    beat_state_t *s = st;
    dsp_biquad_design(&s->lp.c, DSP_BQ_LOWPASS, (float)SAMPLE_RATE, p[BEAT_P_BASS_CUTOFF_HZ], 0.707f, 0.0f);
    s->a_slow = dsp_tau_to_alpha(150.0f, BLOCK_RATE_HZ);
    s->a_mean = dsp_tau_to_alpha(1500.0f, BLOCK_RATE_HZ);
    s->min_onset = (int32_t)(p[BEAT_P_MIN_ONSET_DB] * Q8);
    s->sens_q8 = (int32_t)(p[BEAT_P_SENSITIVITY] * Q8);

    float min_bpm = p[BEAT_P_MIN_BPM];
    float max_bpm = p[BEAT_P_MAX_BPM];
    if (max_bpm < min_bpm + 10.0f) max_bpm = min_bpm + 10.0f;
    const float odf_rate = BLOCK_RATE_HZ / 2.0f;
    int lmin = (int)floorf(60.0f * odf_rate / max_bpm);
    int lmax = (int)ceilf(60.0f * odf_rate / min_bpm);
    if (lmin < 2) lmin = 2;
    if (lmax > HIST / 2) lmax = HIST / 2;
    if (lmax - lmin + 1 > MAX_LAGS) lmax = lmin + MAX_LAGS - 1;
    s->lag_min = (uint16_t)lmin;
    s->lag_max = (uint16_t)lmax;
    /* log-gaussian tempo prior centred on 120 BPM, sigma = 1 octave */
    for (int l = lmin; l <= lmax; l++) {
        float bpm = 60.0f * odf_rate / (float)l;
        float o = log2f(bpm / 120.0f);
        s->prior[l - lmin] = dsp_float_to_q15(expf(-0.5f * o * o));
    }
    s->min_interval_q8 = (int32_t)(60.0f / max_bpm * BLOCK_RATE_HZ * 0.6f * Q8);
    s->lock = p[BEAT_P_TEMPO_LOCK] > 0.5f;
    s->tol_q15 = (uint16_t)dsp_float_to_q15(p[BEAT_P_LOCK_TOLERANCE]);
    s->flywheel_max = (uint8_t)(p[BEAT_P_FLYWHEEL_BEATS] + 0.5f);
}

static void estimate_tempo(beat_state_t *s)
{
    /* linearise history (oldest first), remove mean */
    int32_t sum = 0;
    for (int i = 0; i < HIST; i++) {
        s->lin[i] = s->hist[(s->hpos + i) % HIST];
        sum += s->lin[i];
    }
    int16_t mean = (int16_t)(sum / HIST);
    int64_t r0 = 0;
    for (int i = 0; i < HIST; i++) {
        s->lin[i] = (int16_t)(s->lin[i] - mean);
        r0 += (int32_t)s->lin[i] * s->lin[i];
    }
    r0 >>= 12;
    if (r0 <= 0) return;

    dsp_autocorr_q15(s->lin, HIST, s->lag_min, s->lag_max, s->corr);
    int nl = s->lag_max - s->lag_min + 1;
    int best = -1;
    int64_t best_score = 0;
    for (int i = 0; i < nl; i++) {
        int64_t sc = ((int64_t)s->corr[i] * s->prior[i]) >> 15;
        if (sc > best_score) {
            best_score = sc;
            best = i;
        }
    }
    if (best < 0) return;
    uint16_t conf = (uint16_t)((s->corr[best] > r0) ? 32767 : ((int64_t)s->corr[best] * 32767) / r0);
    s->confidence = conf;
    if (conf < 4000) return;            /* ~0.12: no periodicity */

    /* parabolic interpolation around the peak */
    int32_t lag_q8 = (s->lag_min + best) * Q8;
    if (best > 0 && best < nl - 1) {
        int64_t ym = s->corr[best - 1], y0 = s->corr[best], yp = s->corr[best + 1];
        int64_t den = ym - 2 * y0 + yp;
        if (den < 0) {
            int32_t d = (int32_t)(((ym - yp) * Q8) / (2 * den));
            if (d > -Q8 / 2 && d < Q8 / 2) lag_q8 += d;  /* x0 + (ym-yp)/(2(ym-2y0+yp)) */
        }
    }
    uint32_t period = (uint32_t)lag_q8 * 2u;   /* ODF is decimated by 2 */

    if (s->period_q8 == 0) {
        s->period_q8 = period;
        return;
    }
    int32_t diff = (int32_t)period - (int32_t)s->period_q8;
    if ((uint32_t)(diff < 0 ? -diff : diff) * 10u < s->period_q8) {
        s->period_q8 = (uint32_t)((int32_t)s->period_q8 + diff / 4);     /* within 10 %: smooth */
        s->cand_cnt = 0;
    } else {
        int32_t cd = (int32_t)period - (int32_t)s->cand_q8;
        if (s->cand_cnt && (uint32_t)(cd < 0 ? -cd : cd) * 10u < s->cand_q8) {
            if (++s->cand_cnt >= 3) {   /* consistent new tempo -> switch */
                s->period_q8 = period;
                s->cand_cnt = 0;
            }
        } else {
            s->cand_q8 = period;
            s->cand_cnt = 1;
        }
    }
}

static void process(void *st, audio_frame_t *f)
{
    beat_state_t *s = st;

    /* --- onset detection function --- */
    memcpy(s->scratch, f->samples, f->n * sizeof(q31_t));
    dsp_biquad_process(&s->lp, s->scratch, f->n);
    /* energy over the last E_WIN blocks: one block (4 ms) is shorter than a bass
     * period, so a single block's energy wobbles within one kick */
    s->e_hist[s->e_pos] = dsp_energy(s->scratch, f->n);
    s->e_pos = (uint8_t)((s->e_pos + 1u) % E_WIN);
    uint64_t e = 0;
    for (int i = 0; i < E_WIN; i++) e += s->e_hist[i];
    e /= (uint64_t)E_WIN * f->n;
    int32_t e_db = dsp_power_to_db_q8(e ? e : 1) - 90 * Q8;       /* (Q15 units)^2 -> ~dBFS */
    if (!s->primed) {
        s->e_slow = e_db;
        s->primed = true;
    }
    int32_t odf = e_db - s->e_slow;
    if (odf < 0) odf = 0;
    s->e_slow = dsp_smooth(s->e_slow, e_db, s->a_slow);
    s->odf_mean = dsp_smooth(s->odf_mean, odf, s->a_mean);
    f->onset = odf;

    int32_t thr = (s->odf_mean * s->sens_q8) >> 8;
    if (thr < s->min_onset) thr = s->min_onset;
    /* one onset per event: re-arm only after the ODF fell well below the threshold */
    bool onset = s->armed && (odf > thr);
    if (onset) s->armed = false;
    else if (odf < thr / 2) s->armed = true;
    s->odf_prev = odf;

    /* --- tempo --- */
    s->dec_acc += odf;
    if (++s->dec_n == 2) {
        int32_t v = s->dec_acc / 2;
        s->hist[s->hpos] = (int16_t)(v > INT16_MAX ? INT16_MAX : v);
        s->hpos = (uint16_t)((s->hpos + 1u) % HIST);
        s->dec_acc = 0;
        s->dec_n = 0;
    }
    if (++s->tempo_cnt >= TEMPO_EVERY) {
        s->tempo_cnt = 0;
        estimate_tempo(s);
    }

    /* --- beat decision --- */
    s->t_q8 += Q8;
    bool locked = s->lock && s->period_q8 && s->confidence >= 4000;
    bool emit = false;
    bool predicted = false;
    uint16_t strength = 0;
    if (onset) {
        int32_t st_q15 = (int32_t)(((int64_t)odf * 16384) / thr);     /* thr -> 0.5 */
        strength = (uint16_t)(st_q15 > 32767 ? 32767 : st_q15);
    }
    const uint16_t onset_strength = strength;

    if (onset && s->t_q8 >= s->min_interval_q8) {
        if (!locked) {
            emit = true;
        } else {
            int32_t period = (int32_t)s->period_q8;
            int32_t win = (int32_t)(((int64_t)period * s->tol_q15) >> 15);
            if (s->t_q8 >= period - win) {
                emit = true;                            /* on time (or early): resync */
                s->s_in += ((int32_t)strength - s->s_in) / 4;
            } else if (s->t_q8 <= win) {
                s->s_in += ((int32_t)strength - s->s_in) / 4;
                if (s->last_predicted) {
                    s->t_q8 -= s->t_q8 / 2;             /* late: pull the phase */
                    s->flywheel = 0;
                }
            } else {
                /* Off-phase onset. If onsets keep arriving at the same offset and are
                 * stronger than the in-phase ones we are locked to the wrong phase
                 * (e.g. the off-beat hi-hat): re-acquire on these onsets. */
                int32_t d = s->t_q8 - s->off_ph_q8;
                if (s->off_cnt && (d < 0 ? -d : d) < win) {
                    s->off_cnt++;
                    s->off_ph_q8 = (s->off_ph_q8 + s->t_q8) / 2;
                    s->s_off += ((int32_t)strength - s->s_off) / 2;
                } else {
                    s->off_cnt = 1;
                    s->off_ph_q8 = s->t_q8;
                    s->s_off = strength;
                }
                if (s->off_cnt >= 3 && s->s_off * 2 > s->s_in * 3) {
                    emit = true;
                    s->s_in = s->s_off;
                    s->off_cnt = 0;
                }
            }
        }
    }
    if (!emit && locked && s->t_q8 >= (int32_t)s->period_q8 && s->flywheel < s->flywheel_max) {
        emit = true;
        predicted = true;
        strength = (uint16_t)((s->last_strength * 3u) / 4u);
        s->flywheel++;
        s->s_in -= s->s_in / 4;                         /* no supporting onset */
    }
    if (emit) {
        if (!predicted) {
            s->flywheel = 0;
            s->last_strength = strength;
        }
        s->last_predicted = predicted;
        s->t_q8 = 0;
        s->beat_count++;
    }

    f->beat = emit;
    f->beat_strength = emit ? strength : 0;
    f->beat_count = s->beat_count;
    f->beat_predicted = emit && predicted;
    f->bass_onset = onset;
    f->bass_onset_strength = onset_strength;
    f->tempo_conf = s->confidence;
    f->bpm_x10 = (s->period_q8 && s->confidence >= 4000)
                 ? (uint16_t)((uint32_t)(600.0f * BLOCK_RATE_HZ * Q8) / s->period_q8) : 0;
    if (s->period_q8) {
        uint32_t ph = ((uint32_t)s->t_q8 * 32767u) / s->period_q8;
        f->beat_phase = (uint16_t)(ph > 32767u ? 32767u : ph);
    } else {
        f->beat_phase = 0;
    }
}

const stage_def_t g_stage_beat = {
    .id = STAGE_BEAT,
    .name = "Beat detector",
    .desc = "Bass onsets + autocorrelation tempo + phase-locked beat prediction",
    .params = s_params,
    .num_params = BEAT_P_COUNT,
    .state = &s_st,
    .reset = reset,
    .configure = configure,
    .process = process,
};
