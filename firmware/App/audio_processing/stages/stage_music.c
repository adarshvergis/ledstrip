/**
 * @file stage_music.c
 * @brief Music analysis ("scene director"): what is the music doing right now?
 *
 * Inputs: raw band levels (Spectrum), envelope / level (Envelope, AGC), beats and
 * tempo confidence (Beat), the lead voice (Melody). Outputs audio_frame_t.music:
 *
 * 1. Hits: per band group the spectral flux (dB rise above a 60 ms baseline) is
 *    compared with an adaptive threshold (one onset per event, as in stage_beat).
 *    Kick = the beat stage's bass onset; MID (bands 2-5) = tom, or snare if the upper
 *    mids (6-9) rise at the same time; HIGH (10-11) = hi-hat, upgraded to a crash if it
 *    still rings 100 ms later. While a lone pitched voice plays, mid-range rises are
 *    its notes, not drums.
 * 2. Clarity: tempo confidence x share of the last 8 beats that had a real onset (not
 *    flywheel). High in a groove, collapses in solos, breaks and fills.
 * 3. Intensity: short (300 ms) vs long (8 s) loudness. Build: loudness and treble
 *    share rising for seconds. Drop: after a build or a quiet break the bass comes
 *    back >= 6 dB louder; lasts drop_bars bars.
 * 4. Fill: tom + snare hits in the last 0.5 s far above the beat rate.
 * 5. Layer weights (groove / lead / percussion / ambient), crossfaded.
 * 6. Genre: fixed by the parameter, or (auto) scored once per second from ~10 s
 *    statistics (tempo, kick / snare / hat rates, 808 tails, clarity, salience); a new
 *    genre must win 10 s in a row.
 */
#include "audio_types.h"
#include "music_profiles.h"

#include <string.h>

#define NB          ((int)AUDIO_NUM_BANDS)
#define Q8          256
#define DB(x)       ((int32_t)((x) * 256))
#define Q15F(x)     ((int32_t)((x) * 32767))
#define BPS         ((int32_t)(SAMPLE_RATE / AUDIO_HOP_SIZE))  /* blocks per second */
#define WIN         128                                     /* fill window, blocks (~0.5 s) */
#define TREND_N     5                                       /* 2 s of 0.5 s trend samples */

enum { G_LOW = 0, G_MID, G_HMID, G_HIGH, G_COUNT };

static const uint8_t s_grp_lo[G_COUNT] = {0, 2, 6, 10};
static const uint8_t s_grp_hi[G_COUNT] = {1, 5, 9, 11};
static const int32_t s_grp_rel[G_COUNT] = {DB(12), DB(15), DB(20), DB(24)};  /* max distance to the loudest group */

typedef struct {
    int32_t flux;                       /* dB Q8 */
    int32_t mean;
    int32_t thr;
    bool    armed;
    bool    on;
    uint16_t strength;
} group_t;

typedef struct {
    /* config */
    uint8_t  genre_param;
    int32_t  clarity_thr;               /* Q15 */
    int32_t  lead_sens, fill_sens;      /* Q8 */
    int32_t  hit_db;                    /* Q8 */
    q31_t    a_in, a_out;
    bool     drop_detect;
    uint8_t  drop_bars, bpb;
    q31_t    a_base, a_mean, a_short, a_mid, a_long, a_stat;
    q31_t    a_clar_up, a_clar_dn, a_build;
    /* hits */
    bool     primed;
    int32_t  base[NB];
    group_t  g[G_COUNT];
    uint8_t  win[WIN];                  /* tom + snare hits per block */
    uint8_t  wpos;
    int32_t  wsum;
    uint32_t now;
    uint32_t last_hat;
    uint8_t  roll_cnt;
    uint16_t roll_hold;
    uint16_t crash_recent;
    uint8_t  crash_check;               /* blocks until a loud cymbal hit is checked for ringing */
    int32_t  crash_peak;
    uint16_t crash_str;
    /* beat structure */
    uint8_t  beat_bits;
    uint8_t  fly_check;                 /* blocks left to confirm a flywheel beat by a kick */
    uint32_t last_kick;
    uint8_t  beat_n;
    uint32_t since_beat;
    int32_t  clarity;
    int32_t  bass;                      /* bass presence, Q15 */
    int32_t  acc[8];                    /* average beat strength per bar position */
    uint8_t  bar_beat;
    uint32_t bar_count;
    /* loudness / sections */
    bool     loud_primed;
    int32_t  trend_e[TREND_N], trend_h[TREND_N];    /* e_mid / brightness every 0.5 s */
    uint8_t  trend_pos;
    uint16_t trend_tick;
    uint16_t warm;
    int32_t  e_short, e_mid, e_long;    /* dB Q8 */
    int32_t  hs_fast, hs_slow;          /* treble share, dB Q8 */
    int32_t  low_mid;
    int32_t  build;
    uint16_t quiet_cnt;
    uint16_t build_cnt;
    uint16_t silent_cnt;
    uint16_t drop_armed;
    uint16_t drop_beats;
    uint16_t drop_timeout;
    bool     drop;
    uint8_t  drop_pend, drop_ok;        /* drop candidate being confirmed */
    bool     drop_beat;
    int32_t  drop_ref;
    uint16_t fill_hold;
    int32_t  note_act;                  /* recent note starts of the lead voice, Q15 */
    int32_t  w[4];                      /* groove, lead, perc, ambient */
    /* auto genre statistics (Hz / ratios in Q8, levels Q15) */
    int32_t  st_bpm, st_clar, st_sal, st_bass, st_roll, st_tail;
    int32_t  st_rate[HIT_COUNT];
    bool     tail_on;
    int32_t  tail_peak;
    uint16_t tail_cnt;
    uint16_t stat_tick;
    uint16_t music_s;
    uint8_t  auto_genre, cand_genre, cand_cnt;
    bool     auto_decided;
} music_state_t;

static music_state_t s_st;

static const param_desc_t s_params[MUSIC_P_COUNT] = {
    [MUSIC_P_GENRE]         = PDESC_E("genre", GENRE_ENUM_OPTS, GENRE_COUNT, 0.0f),
    [MUSIC_P_CLARITY]       = PDESC_F("clarity_threshold", "", 0.05f, 0.9f, 0.35f, 0.01f),
    [MUSIC_P_LEAD_SENS]     = PDESC_F("lead_sensitivity", "x", 0.0f, 3.0f, 1.0f, 0.05f),
    [MUSIC_P_FILL_SENS]     = PDESC_F("fill_sensitivity", "x", 0.0f, 3.0f, 1.0f, 0.05f),
    [MUSIC_P_HIT_DB]        = PDESC_F("hit_threshold", "dB", 1.0f, 15.0f, 4.0f, 0.1f),
    [MUSIC_P_FADE_MS]       = PDESC_I("scene_fade", "ms", 20.0f, 3000.0f, 250.0f),
    [MUSIC_P_DROP_DETECT]   = PDESC_B("drop_detect", 1.0f),
    [MUSIC_P_DROP_BARS]     = PDESC_I("drop_bars", "bars", 1.0f, 32.0f, 8.0f),
    [MUSIC_P_BEATS_PER_BAR] = PDESC_I("beats_per_bar", "beats", 2.0f, 8.0f, 4.0f),
};

static int32_t clamp15(int32_t v)
{
    return v < 0 ? 0 : (v > 32767 ? 32767 : v);
}

static int32_t max3(int32_t a, int32_t b, int32_t c)
{
    int32_t m = a > b ? a : b;
    return m > c ? m : c;
}

static void reset(void *st)
{
    music_state_t *s = st;
    s->primed = false;
    memset(s->g, 0, sizeof(s->g));
    for (int i = 0; i < G_COUNT; i++) {
        s->g[i].armed = true;
        s->g[i].mean = DB(1);
    }
    memset(s->win, 0, sizeof(s->win));
    s->wpos = 0;
    s->wsum = 0;
    s->now = 0;
    s->last_hat = 0;
    s->roll_cnt = 0;
    s->roll_hold = 0;
    s->crash_recent = 0;
    s->crash_check = 0;
    s->crash_peak = 0;
    s->crash_str = 0;
    s->beat_bits = 0;
    s->fly_check = 0;
    s->last_kick = 0;
    s->beat_n = 0;
    s->since_beat = 0;
    s->clarity = 0;
    s->bass = 0;
    memset(s->acc, 0, sizeof(s->acc));
    s->bar_beat = 0xFF;                 /* the first beat becomes the downbeat */
    s->bar_count = 0;
    s->loud_primed = false;
    s->trend_pos = 0;
    s->trend_tick = 0;
    s->warm = 0;
    s->build = 0;
    s->quiet_cnt = 0;
    s->build_cnt = 0;
    s->silent_cnt = 0;
    s->drop_armed = 0;
    s->drop_beats = 0;
    s->drop_timeout = 0;
    s->drop = false;
    s->drop_pend = 0;
    s->drop_ok = 0;
    s->drop_beat = false;
    s->drop_ref = 0;
    s->fill_hold = 0;
    s->note_act = 0;
    memset(s->w, 0, sizeof(s->w));
    s->st_bpm = s->st_clar = s->st_sal = s->st_bass = s->st_roll = s->st_tail = 0;
    memset(s->st_rate, 0, sizeof(s->st_rate));
    s->tail_on = false;
    s->tail_peak = 0;
    s->tail_cnt = 0;
    s->stat_tick = 0;
    s->music_s = 0;
    s->auto_genre = GENRE_POP;
    s->cand_genre = GENRE_POP;
    s->cand_cnt = 0;
    s->auto_decided = false;
}

static void configure(void *st, const float *p)
{
    music_state_t *s = st;
    s->genre_param = (uint8_t)(p[MUSIC_P_GENRE] + 0.5f);
    s->clarity_thr = dsp_float_to_q15(p[MUSIC_P_CLARITY]);
    s->lead_sens = (int32_t)(p[MUSIC_P_LEAD_SENS] * Q8);
    s->fill_sens = (int32_t)(p[MUSIC_P_FILL_SENS] * Q8);
    s->hit_db = (int32_t)(p[MUSIC_P_HIT_DB] * Q8);
    s->a_in = dsp_tau_to_alpha(p[MUSIC_P_FADE_MS], BLOCK_RATE_HZ);
    s->a_out = dsp_tau_to_alpha(2.0f * p[MUSIC_P_FADE_MS], BLOCK_RATE_HZ);
    s->drop_detect = p[MUSIC_P_DROP_DETECT] > 0.5f;
    s->drop_bars = (uint8_t)p[MUSIC_P_DROP_BARS];
    s->bpb = (uint8_t)p[MUSIC_P_BEATS_PER_BAR];
    s->a_base = dsp_tau_to_alpha(60.0f, BLOCK_RATE_HZ);
    s->a_mean = dsp_tau_to_alpha(1000.0f, BLOCK_RATE_HZ);
    s->a_short = dsp_tau_to_alpha(300.0f, BLOCK_RATE_HZ);
    s->a_mid = dsp_tau_to_alpha(2000.0f, BLOCK_RATE_HZ);
    s->a_long = dsp_tau_to_alpha(8000.0f, BLOCK_RATE_HZ);
    s->a_stat = dsp_tau_to_alpha(10000.0f, BLOCK_RATE_HZ);
    s->a_clar_up = dsp_tau_to_alpha(300.0f, BLOCK_RATE_HZ);
    s->a_clar_dn = dsp_tau_to_alpha(800.0f, BLOCK_RATE_HZ);
    s->a_build = dsp_tau_to_alpha(300.0f, BLOCK_RATE_HZ);
}

static int32_t group_db(const audio_frame_t *f, int g)
{
    int32_t sum = 0;
    for (int b = s_grp_lo[g]; b <= s_grp_hi[g]; b++) sum += f->band_db[b];
    return sum / (s_grp_hi[g] - s_grp_lo[g] + 1);
}

/* ---- 1. hits ------------------------------------------------------------- */

static uint8_t detect_hits(music_state_t *s, audio_frame_t *f)
{
    music_t *m = &f->music;
    if (!s->primed) {
        for (int b = 0; b < NB; b++) s->base[b] = f->band_db[b];
        s->primed = true;
    }
    /* dB rises are scale free: ignore bands far below the loudest one, where a tiny
     * absolute change (leakage, noise) would look like a big hit */
    int32_t top = f->band_db[0];
    for (int b = 1; b < NB; b++) {
        if (f->band_db[b] > top) top = f->band_db[b];
    }
    int32_t d[NB];
    for (int b = 0; b < NB; b++) {
        int32_t x = f->band_db[b] - s->base[b];
        d[b] = (x > 0 && f->band_db[b] > top - DB(30)) ? x : 0;
        s->base[b] = dsp_smooth(s->base[b], f->band_db[b], s->a_base);
    }
    /* a hit must also be loud next to the rest of the spectrum (not a click in a quiet band) */
    int32_t gdb[G_COUNT], gtop = INT32_MIN;
    for (int i = 0; i < G_COUNT; i++) {
        gdb[i] = f->band_db[s_grp_lo[i]];          /* loudest band (groups differ in width) */
        for (int b = s_grp_lo[i] + 1; b <= s_grp_hi[i]; b++) {
            if (f->band_db[b] > gdb[i]) gdb[i] = f->band_db[b];
        }
        if (gdb[i] > gtop) gtop = gdb[i];
    }
    for (int i = 0; i < G_COUNT; i++) {
        group_t *g = &s->g[i];
        int32_t sum = 0;
        for (int b = s_grp_lo[i]; b <= s_grp_hi[i]; b++) sum += d[b];
        g->flux = sum / (s_grp_hi[i] - s_grp_lo[i] + 1);
        g->thr = (g->mean * 460) >> 8;                  /* 1.8 x mean */
        if (g->thr < s->hit_db) g->thr = s->hit_db;
        g->on = g->armed && g->flux > g->thr && gdb[i] > gtop - s_grp_rel[i];
        if (g->on) g->armed = false;
        else if (g->flux < g->thr / 2) g->armed = true;
        g->mean = dsp_smooth(g->mean, g->flux, s->a_mean);
        int32_t str = (int32_t)(((int64_t)g->flux * 16384) / g->thr);
        g->strength = (uint16_t)clamp15(str);
    }

    uint8_t hits = 0;
    const group_t *lo = &s->g[G_LOW], *mid = &s->g[G_MID], *hm = &s->g[G_HMID], *hi = &s->g[G_HIGH];
    /* while a single pitched voice dominates and there is no groove going on, rises
     * above the bass are its notes and harmonics, not drums (with a groove, e.g. a solo
     * over a band, the drums keep being detected) */
    bool tonal = f->melody.active && f->melody.salience > Q15F(0.5) && s->clarity < Q15F(0.3);
    bool snare = false, tom = false;
    /* kick / 808: the beat stage's bass onset (low-passed time signal; the 256-point FFT
     * cannot resolve 50 Hz); with a clearly stronger rise in the mids it is a low tom */
    if (f->bass_onset && !(tonal && lo->flux < mid->flux)) {
        if (!(mid->on && mid->flux > 2 * lo->flux)) {
            hits |= 1u << HIT_KICK;
            m->hit_strength[HIT_KICK] = f->bass_onset_strength;
        } else {
            tom = true;
            m->hit_strength[HIT_TOM] = f->bass_onset_strength;
        }
    }
    if (tonal) {
        /* nothing in the mid range */
    } else if (mid->on && mid->flux * 5 >= lo->flux * 4) {     /* else: the kick leaking upwards */
        if (hm->flux * 5 > hm->thr * 3 && mid->flux * 2 >= hi->flux) {
            snare = true;                               /* body + crack: snare */
            m->hit_strength[HIT_SNARE] = mid->strength;
        } else if (!(hits & (1u << HIT_KICK))) {
            tom = true;
            m->hit_strength[HIT_TOM] = mid->strength;
        }
    } else if (hm->on && hi->flux * 2 > hi->thr && mid->flux * 2 > mid->thr) {
        snare = true;                                   /* clap */
        m->hit_strength[HIT_SNARE] = hm->strength;
    }
    if (tom && !snare) {
        hits |= 1u << HIT_TOM;
        int32_t num = 0, den = 0;
        for (int b = 1; b <= 6; b++) {
            num += (b - 1) * d[b];
            den += d[b];
        }
        m->tom_pos = (uint16_t)(den ? clamp15((int32_t)(((int64_t)num * 32767) / (den * 5))) : 16384);
    }
    if (snare) hits |= 1u << HIT_SNARE;
    /* cymbals: every hit is a hi-hat at once; if it still rings 100 ms later (a hat is
     * gone by then) it becomes a crash */
    if (s->crash_check) {
        if (gdb[G_HIGH] > s->crash_peak) s->crash_peak = gdb[G_HIGH];
        if (--s->crash_check == 0 && gdb[G_HIGH] > s->crash_peak - DB(6)) {
            hits |= 1u << HIT_CRASH;
            m->hit_strength[HIT_CRASH] = s->crash_str;
            s->crash_recent = 37;
        }
    }
    if (hi->on && !snare && !tonal) {
        hits |= 1u << HIT_HAT;
        m->hit_strength[HIT_HAT] = hi->strength;
        if (hi->strength > 20000 && !s->crash_check) {
            s->crash_check = (uint8_t)(BPS / 10);
            s->crash_peak = gdb[G_HIGH];
            s->crash_str = hi->strength;
        }
    }
    return hits;
}

/* ---- 2. beat structure ---------------------------------------------------- */

static void track_beats(music_state_t *s, const audio_frame_t *f, uint32_t period)
{
    /* a flywheel beat with a kick within 80 ms still is a real beat (the beat tracker's
     * phase can lag behind for a few beats, e.g. after a fill) */
    const uint32_t near = (uint32_t)(BPS * 2 / 25);
    if (f->music.hits & (1u << HIT_KICK)) {
        s->last_kick = s->now;
        if (s->fly_check) {
            s->beat_bits |= 1u;
            s->fly_check = 0;
        }
    }
    if (s->fly_check) s->fly_check--;
    s->since_beat++;
    if (f->beat) {
        bool supported = !f->beat_predicted || s->now - s->last_kick <= near;
        s->beat_bits = (uint8_t)((s->beat_bits << 1) | (supported ? 1u : 0u));
        s->fly_check = supported ? 0 : (uint8_t)near;
        if (s->beat_n < 8) s->beat_n++;
        s->since_beat = 0;

        s->bar_beat = (s->bar_beat + 1u >= s->bpb) ? 0 : (uint8_t)(s->bar_beat + 1u);
        if (s->bar_beat == 0) s->bar_count++;
        /* downbeat alignment: the bar position with clearly stronger beats is "1" */
        int32_t str = f->beat_strength + (s->crash_recent ? 16384 : 0);
        s->acc[s->bar_beat] += (str - s->acc[s->bar_beat]) / 8;
        if (s->bar_beat == s->bpb - 1) {
            uint8_t best = 0;
            for (uint8_t i = 1; i < s->bpb; i++) {
                if (s->acc[i] > s->acc[best]) best = i;
            }
            if (best && s->acc[best] * 10 > s->acc[0] * 13) {
                int32_t tmp[8];
                for (uint8_t i = 0; i < s->bpb; i++) tmp[i] = s->acc[(i + best) % s->bpb];
                memcpy(s->acc, tmp, sizeof(tmp[0]) * s->bpb);
                s->bar_beat = (uint8_t)((s->bpb - 1u + s->bpb - best) % s->bpb);
            }
        }
        if (s->drop && s->drop_beats && --s->drop_beats == 0) s->drop = false;
    } else if (s->since_beat > 2 * period) {
        s->beat_bits <<= 1;                             /* a missing beat */
        s->since_beat = period;
    }
    int32_t supp = (__builtin_popcount(s->beat_bits) * 32767) / (s->beat_n > 4 ? s->beat_n : 4);
    int32_t conf = clamp15(((int32_t)f->tempo_conf - 4000) * 32767 / 8000);
    /* a "beat" needs a bass line / kick: a melody alone can fool the tempo tracker */
    int32_t low = group_db(f, G_LOW);
    int32_t top = max3(low, group_db(f, G_MID), group_db(f, G_HMID));
    s->bass = dsp_smooth(s->bass, clamp15(((low - (top - DB(20))) * 32767) / DB(10)), s->a_build);
    int32_t raw = f->bpm_x10 ? (int32_t)(((int64_t)conf * supp) >> 15) : 0;
    raw = (raw * s->bass) >> 15;
    s->clarity = dsp_smooth(s->clarity, raw, raw > s->clarity ? s->a_clar_up : s->a_clar_dn);
}

/* ---- 6. genre ------------------------------------------------------------- */

/* The statistics are long averages (10 s); they keep 8 extra fraction bits (ST_x) so
 * a single hit still moves them. Rates: hits per second, Q16. */
#define ST_UP(x)    ((int32_t)(x) * 256)
#define ST(x)       ((x) / 256)

static uint8_t score_genre(const music_state_t *s)
{
    int32_t bpm = ST(s->st_bpm) / 10;
    int32_t bhz = bpm ? (bpm * Q8) / 60 : 2 * Q8;      /* beat rate, Hz Q8 */
    int32_t rate[HIT_COUNT], hits = 0;                  /* Hz Q8 */
    for (int h = 0; h < HIT_COUNT; h++) {
        rate[h] = s->st_rate[h] / 256;
        hits += rate[h];
    }
    int32_t kick = (rate[HIT_KICK] * Q8) / bhz;         /* per beat, Q8 */
    int32_t snare = (rate[HIT_SNARE] * Q8) / bhz;
    int32_t hat = (rate[HIT_HAT] * Q8) / bhz;
    bool backbeat = snare > Q8 * 35 / 100 && snare < Q8 * 65 / 100;
    int32_t clar = ST(s->st_clar), sal = ST(s->st_sal), roll = ST(s->st_roll), bass = ST(s->st_bass);
    int32_t tail = s->st_tail;                          /* blocks, Q8 */
    int sc[GENRE_COUNT] = {0};

    sc[GENRE_EDM] = 2 * (clar > Q15F(0.6)) + 2 * (bpm >= 118 && bpm <= 135) + 2 * (kick >= Q8 * 8 / 10) +
                    (bass > DB(6));
    sc[GENRE_TRAP] = ((bpm >= 60 && bpm <= 85) || (bpm >= 130 && bpm <= 170 && kick < Q8 * 6 / 10)) +
                     2 * (roll > Q15F(0.1)) + 3 * (tail > 90 * Q8) + (hat >= 2 * Q8);
    sc[GENRE_HIPHOP] = 2 * (bpm >= 80 && bpm <= 100) + 2 * backbeat + (bass > DB(6)) +
                       (tail > 40 * Q8 && tail <= 90 * Q8);
    sc[GENRE_RNB] = (bpm >= 60 && bpm <= 100) + 2 * (sal > Q15F(0.3)) + (hat < Q8) +
                    (clar > Q15F(0.3) && clar < Q15F(0.7)) + (bass > DB(6));
    sc[GENRE_ROCK] = (bpm >= 100 && bpm <= 160) + 2 * backbeat + (rate[HIT_TOM] > Q8 / 5) +
                     (rate[HIT_CRASH] > Q8 / 10) + (bass < DB(6)) + (kick < Q8 * 8 / 10);
    sc[GENRE_METAL] = 3 * (bpm > 150 || kick > Q8 * 3 / 2) + (snare > Q8 * 8 / 10) +
                      (rate[HIT_HAT] + rate[HIT_CRASH] > 2 * Q8) + (bass < DB(3));
    sc[GENRE_ORCHESTRAL] = 3 * (clar < Q15F(0.25)) + (sal > Q15F(0.3)) + 2 * (hits < Q8);
    sc[GENRE_POP] = 3 + (bpm >= 95 && bpm <= 130) + (clar > Q15F(0.4));

    uint8_t best = GENRE_POP;
    for (uint8_t g = GENRE_EDM; g < GENRE_COUNT; g++) {
        if (sc[g] > sc[best]) best = g;
    }
    return best;
}

static void update_stats(music_state_t *s, const audio_frame_t *f, uint8_t hits, bool silent)
{
    /* kick / 808 tail: time until the bass band fell 12 dB below its peak after a kick */
    if (s->tail_on) {
        int32_t b0 = f->band_db[0];
        if (b0 > s->tail_peak) s->tail_peak = b0;
        if (b0 < s->tail_peak - DB(12) || ++s->tail_cnt >= BPS || (hits & (1u << HIT_KICK))) {
            s->st_tail += ((int32_t)s->tail_cnt * Q8 - s->st_tail) / 8;
            s->tail_on = false;
        }
    }
    if (hits & (1u << HIT_KICK)) {
        s->tail_on = true;
        s->tail_peak = f->band_db[0];
        s->tail_cnt = 0;
    }
    if (silent) return;

    const q31_t a = s->a_stat;
    if (f->bpm_x10) s->st_bpm = s->st_bpm ? dsp_smooth(s->st_bpm, ST_UP(f->bpm_x10), a) : ST_UP(f->bpm_x10);
    s->st_clar = dsp_smooth(s->st_clar, ST_UP(s->clarity), a);
    s->st_sal = dsp_smooth(s->st_sal, ST_UP(f->melody.salience), a);
    s->st_roll = dsp_smooth(s->st_roll, f->music.hat_roll ? ST_UP(32767) : 0, a);
    int32_t all = 0;
    for (int b = 0; b < NB; b++) all += f->band_db[b];
    s->st_bass = dsp_smooth(s->st_bass, ST_UP(group_db(f, G_LOW) - all / NB), a);
    for (int h = 0; h < HIT_COUNT; h++) {
        s->st_rate[h] = dsp_smooth(s->st_rate[h], (hits & (1u << h)) ? (BPS << 16) : 0, a);
    }

    if (++s->stat_tick < BPS) return;                  /* once per second */
    s->stat_tick = 0;
    if (s->music_s < 0xFFFF) s->music_s++;
    if (s->music_s < 8) return;
    uint8_t g = score_genre(s);
    if (g == s->cand_genre) {
        if (s->cand_cnt < 255) s->cand_cnt++;
    } else {
        s->cand_genre = g;
        s->cand_cnt = 1;
    }
    if (s->cand_cnt >= (s->auto_decided ? 10 : 3)) {
        s->auto_genre = s->cand_genre;
        s->auto_decided = true;
    }
}

/* ---- main ----------------------------------------------------------------- */

static void process(void *st, audio_frame_t *f)
{
    music_state_t *s = st;
    music_t *m = &f->music;
    s->now++;
    uint8_t genre = s->genre_param != GENRE_AUTO ? s->genre_param : s->auto_genre;
    const music_profile_t *pr = music_profile(genre);
    uint32_t period = f->bpm_x10 ? (uint32_t)(600 * BPS) / f->bpm_x10 : (uint32_t)BPS / 2u;

    /* hits */
    uint8_t hits = f->spectrum_valid ? detect_hits(s, f) : 0;
    if (s->crash_recent) s->crash_recent--;
    if (hits & (1u << HIT_HAT)) {
        uint32_t dt = s->now - s->last_hat;
        if (dt * 5 <= period && dt <= 40) {
            if (s->roll_cnt < 255) s->roll_cnt++;
        } else {
            s->roll_cnt = 0;
        }
        s->last_hat = s->now;
        if (s->roll_cnt >= 3) s->roll_hold = 50;
    }
    if (s->roll_hold) s->roll_hold--;
    m->hits = hits;
    m->hat_roll = s->roll_hold > 0;
    m->sub = f->bands[0];

    /* fill window: tom + snare hits in the last WIN blocks */
    uint8_t n_fill = (uint8_t)(((hits >> HIT_TOM) & 1u) + ((hits >> HIT_SNARE) & 1u));
    s->wsum += n_fill - s->win[s->wpos];
    s->win[s->wpos] = n_fill;
    s->wpos = (uint8_t)((s->wpos + 1u) % WIN);

    /* beats, clarity, bar */
    track_beats(s, f, period);

    /* silence */
    if (f->level == 0) {
        if (s->silent_cnt < 0xFFFF) s->silent_cnt++;
    } else {
        s->silent_cnt = 0;
    }
    bool silent = s->silent_cnt > BPS / 2;
    if (s->silent_cnt > 2 * BPS) s->loud_primed = false;    /* music after a pause starts fresh */

    /* loudness / sections (trackers start with the music, not with the silence before it) */
    int32_t e = f->envelope_db;
    int32_t low = group_db(f, G_LOW);
    int32_t top = max3(low, group_db(f, G_MID), group_db(f, G_HMID));
    int32_t hs = (group_db(f, G_HMID) + group_db(f, G_HIGH)) / 2 - group_db(f, G_MID);   /* brightness */
    if (!s->loud_primed && f->level > 0) {
        s->e_short = s->e_mid = s->e_long = e;
        s->hs_fast = s->hs_slow = hs;
        s->low_mid = low;
        s->loud_primed = true;
        s->warm = (uint16_t)(2 * BPS);
    }
    s->e_short = dsp_smooth(s->e_short, e, s->a_short);
    s->e_mid = dsp_smooth(s->e_mid, e, s->a_mid);
    s->e_long = dsp_smooth(s->e_long, e, s->a_long);
    s->hs_fast = dsp_smooth(s->hs_fast, hs, s->a_mid);
    s->hs_slow = dsp_smooth(s->hs_slow, hs, s->a_long);
    if (s->warm) {                      /* first 2 s: the slow trackers follow the fast ones */
        s->warm--;
        s->e_long = s->e_mid = s->e_short;
        s->hs_slow = s->hs_fast;
        for (int i = 0; i < TREND_N; i++) {
            s->trend_e[i] = s->e_short;
            s->trend_h[i] = s->hs_fast;
        }
    }
    int32_t low_jump = low - s->low_mid;
    s->low_mid = dsp_smooth(s->low_mid, low, s->a_mid);
    int32_t intensity = clamp15(((s->e_short - s->e_long + DB(6)) * 32767) / DB(12));

    /* build: louder AND brighter over seconds, with the kick thinning out (or a snare roll) */
    int32_t bhz_q8 = (BPS * Q8) / (int32_t)period;
    /* 2 s trends of loudness and brightness (the slow trackers remember the louder part
     * before a break, so compare with the recent past instead) */
    if (++s->trend_tick >= BPS / 2) {
        s->trend_tick = 0;
        s->trend_pos = (uint8_t)((s->trend_pos + 1u) % TREND_N);
        s->trend_e[s->trend_pos] = s->e_short;
        s->trend_h[s->trend_pos] = s->hs_fast;
    }
    uint8_t old = (uint8_t)((s->trend_pos + 1u) % TREND_N);
    int32_t loud_up = clamp15(((s->e_short - s->trend_e[old]) * 32767) / DB(2));
    int32_t bright_up = clamp15(((s->hs_fast - s->trend_h[old]) * 32767) / DB(3));
    int32_t build_raw = (loud_up * bright_up) >> 15;
    if (!s->loud_primed) build_raw = 0;
    bool kick_thin = s->clarity < Q15F(0.5);
    bool snare_roll = s->wsum >= 4;
    if (!kick_thin && !snare_roll) build_raw = (build_raw * 2) / 5;
    s->build = dsp_smooth(s->build, build_raw, s->a_build);
    if (s->build > Q15F(0.5)) {
        if (s->build_cnt < 0xFFFF) s->build_cnt++;
    } else if (s->build < Q15F(0.35)) {
        s->build_cnt = 0;
    }
    bool building = s->build_cnt > 3 * BPS / 2;        /* a build-up lasts bars, a fill one */
    if (building && !s->drop) s->drop_armed = (uint16_t)(4 * BPS);
    if (intensity < Q15F(0.3) && !silent) {
        if (s->quiet_cnt < 0xFFFF) s->quiet_cnt++;
        if (s->quiet_cnt > 2 * BPS && !s->drop) s->drop_armed = (uint16_t)(4 * BPS);
    } else {
        s->quiet_cnt = 0;
    }

    /* drop: the bass comes back hard after a build / break. Candidate on a big kick,
     * confirmed when the bass stays up for ~40 ms (a noise spike in the narrow bass band
     * of a riser does not) */
    bool big_kick = (hits & (1u << HIT_KICK)) && m->hit_strength[HIT_KICK] > Q15F(0.6);
    if (s->drop_pend) {
        if (f->beat) s->drop_beat = true;
        if (low > s->drop_ref + DB(6)) s->drop_ok++;
        if (--s->drop_pend == 0 && s->drop_ok >= (uint8_t)(BPS / 25)) {
            s->drop = true;
            s->drop_beats = (uint16_t)(s->drop_bars * s->bpb);
            s->drop_timeout = (uint16_t)(20 * BPS);
            s->drop_armed = 0;
            s->build_cnt = 0;
            if (s->drop_beat) {                         /* the drop is beat 1 */
                s->bar_beat = 0;
                s->bar_count++;
            } else {
                s->bar_beat = (uint8_t)(s->bpb - 1u);
            }
        }
    } else if (s->drop_detect && s->drop_armed && !s->drop && big_kick && low_jump > DB(6) && low > top - DB(3) &&
               s->e_short > s->e_long - DB(2)) {
        s->drop_pend = (uint8_t)(BPS * 3 / 50);       /* 60 ms */
        s->drop_ref = low - low_jump;                   /* bass level before the kick */
        s->drop_ok = 0;
        s->drop_beat = f->beat;
    }
    if (s->drop_armed) s->drop_armed--;
    if (s->drop && (s->drop_timeout == 0 || --s->drop_timeout == 0)) s->drop = false;

    /* fill: dense tom / snare hits compared with the beat rate */
    int32_t thr = (bhz_q8 * 5 / 2 * WIN) / BPS;       /* 2.5 hits per beat over the window, Q8 hits */
    if (thr < 5 * Q8 / 2) thr = 5 * Q8 / 2;
    int32_t gain = (s->fill_sens * pr->fill_gain) >> 8;
    int32_t fill = 0;
    if (gain > 0) {
        int32_t ratio = (int32_t)(((int64_t)s->wsum * Q8 * 32767 * gain / Q8) / thr);
        fill = clamp15(((ratio - Q15F(0.6)) * 2));
    }
    if (fill > Q15F(0.6)) s->fill_hold = (uint16_t)(BPS * 2 / 5);
    else if (s->fill_hold) s->fill_hold--;
    if (s->fill_hold && fill < Q15F(0.8)) fill = Q15F(0.8);

    /* layer targets */
    /* lead: a dominant voice that moves (a held pad / drone only counts a little) */
    if (f->melody.note_on) s->note_act = clamp15(s->note_act + 32767 / 3);
    else s->note_act -= s->note_act / (int32_t)(3 * BPS / 2) + 1;
    if (s->note_act < 0) s->note_act = 0;
    int32_t lead = 0;
    if (f->melody.active) {
        lead = clamp15(((int32_t)f->melody.salience - Q15F(0.2)) * 5 / 2);
        lead = (lead * clamp15(Q15F(0.4) + s->note_act)) >> 15;
        lead = clamp15((int32_t)(((int64_t)lead * pr->lead_gain * s->lead_sens) >> 16));
    }
    int32_t cthr = s->clarity_thr + pr->clarity_off;
    int32_t groove = clamp15(16384 + ((s->clarity - cthr) * 32767) / Q15F(0.15));
    groove = (groove * (32767 - lead / 2)) >> 15;
    groove = (groove * (32767 - fill * 7 / 10)) >> 15;
    if (s->drop) {                      /* the drop belongs to the beat, at once */
        groove = 32767;
        fill /= 2;
        if (s->w[0] < Q15F(0.8)) s->w[0] = Q15F(0.8);
    }
    int32_t amb = 32767 - max3(groove, lead, fill);
    int32_t tgt[4] = {groove, lead, fill, amb};
    for (int i = 0; i < 4; i++) {
        if (silent) tgt[i] = 0;
        s->w[i] = dsp_smooth(s->w[i], tgt[i], tgt[i] > s->w[i] ? s->a_in : s->a_out);
    }

    m->w_groove = (uint16_t)clamp15(s->w[0]);
    m->w_lead = (uint16_t)clamp15(s->w[1]);
    m->w_perc = (uint16_t)clamp15(s->w[2]);
    m->w_ambient = (uint16_t)clamp15(s->w[3]);
    m->intensity = (uint16_t)intensity;
    m->clarity = (uint16_t)clamp15(s->clarity);
    m->build = building ? (uint16_t)clamp15(s->build) : 0;
    m->drop = s->drop;
    m->bar_beat = s->bar_beat < s->bpb ? s->bar_beat : 0;
    m->bar_count = s->bar_count;
    m->genre = genre == GENRE_AUTO ? GENRE_POP : genre;

    if (silent) m->scene = SCENE_SILENCE;
    else if (s->drop) m->scene = SCENE_DROP;
    else if (building) m->scene = SCENE_BUILD;
    else if (m->w_perc > Q15F(0.4) && m->w_perc >= m->w_groove) m->scene = SCENE_FILL;
    else if (m->w_lead > Q15F(0.4) && m->w_lead > m->w_groove) m->scene = SCENE_LEAD;
    else if (m->w_groove > Q15F(0.4)) m->scene = SCENE_GROOVE;
    else m->scene = SCENE_AMBIENT;

    update_stats(s, f, hits, silent);
}

const stage_def_t g_stage_music = {
    .id = STAGE_MUSIC,
    .name = "Music analysis",
    .desc = "Drum hits, beat clarity, groove / lead / fill / build / drop scenes, genre",
    .params = s_params,
    .num_params = MUSIC_P_COUNT,
    .state = &s_st,
    .reset = reset,
    .configure = configure,
    .process = process,
};
