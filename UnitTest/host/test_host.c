/**
 * @file test_host.c
 * @brief Host (PC) sanity tests of the DSP library and the processing stages.
 *
 * Built with the native compiler by Scripts/firmware/run_firmware_unit_tests.sh; feeds synthetic audio
 * (tones, kick-drum patterns) through the real stage code and checks the results.
 * Not a full unit-test suite - a quick regression net for the signal processing.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_types.h"
#include "hw_common.h"
#include "led_color.h"
#include "music_profiles.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int s_fails;
static int s_checks;

int test_modules_run(int *checks, int *fails);     /* test_modules.c */

#define CHECK(cond, ...)                                    \
    do {                                                    \
        s_checks++;                                         \
        if (cond) {                                         \
            printf("  ok    ");                             \
        } else {                                            \
            printf("  FAIL  ");                             \
            s_fails++;                                      \
        }                                                   \
        printf(__VA_ARGS__);                                \
        printf("\n");                                       \
    } while (0)

static led_rgb_t s_leds[LED_MAX_COUNT];

static void stage_defaults(const stage_def_t *s, float *p)
{
    for (uint8_t i = 0; i < s->num_params; i++) p[i] = s->params[i].def;
}

static void stage_start(const stage_def_t *s, const float *p)
{
    s->configure(s->state, p);
    s->reset(s->state);
}

static void frame_init(audio_frame_t *f)
{
    memset(f, 0, sizeof(*f));
    f->n = AUDIO_HOP_SIZE;
    f->leds = s_leds;
    f->num_leds = LED_MAX_COUNT;
}

static double frand(void)
{
    return (double)rand() / RAND_MAX * 2.0 - 1.0;
}

static q31_t to_q31(double v)
{
    if (v > 0.999999) v = 0.999999;
    if (v < -1.0) v = -1.0;
    return (q31_t)(v * 2147483648.0);
}

/* -------------------------------------------------------------------------- */

static void test_crc(void)
{
    printf("CRC-32\n");
    const char *s = "123456789";
    CHECK(hw_crc32(0, s, 9) == 0xCBF43926u, "check value 0x%08X", (unsigned)hw_crc32(0, s, 9));
    uint32_t c = hw_crc32(0, s, 4);
    c = hw_crc32(c, s + 4, 5);
    CHECK(c == 0xCBF43926u, "chained computation");
}

static void test_math(void)
{
    printf("Fixed-point math\n");
    CHECK(dsp_isqrt64(1000000ull) == 1000, "isqrt(1e6) = %u", (unsigned)dsp_isqrt64(1000000ull));
    double db = dsp_power_to_db_q8(100000000ull) / 256.0;
    CHECK(fabs(db - 80.0) < 0.1, "10log10(1e8) = %.3f dB", db);
    db = dsp_amp_to_db_q8(1ull << 31) / 256.0;
    CHECK(fabs(db - 186.64) < 0.1, "20log10(2^31) = %.3f dB", db);
    uint32_t lin = dsp_db_q8_to_lin_q16((int32_t)(-6.0206 * 256));
    CHECK(abs((int)lin - 32768) < 200, "10^(-6.02/20) = %.4f", lin / 65536.0);
    lin = dsp_db_q8_to_lin_q16(20 * 256);
    CHECK(fabs(lin / 65536.0 - 10.0) < 0.05, "10^(20/20) = %.4f", lin / 65536.0);

    static q31_t buf[256];
    for (int i = 0; i < 256; i++) buf[i] = to_q31(0.5 * sin(2 * M_PI * 8 * i / 256.0));
    double rms = dsp_rms(buf, 256) / 2147483648.0;
    CHECK(fabs(rms - 0.5 / sqrt(2.0)) < 0.002, "rms of 0.5 sine = %.4f", rms);

    for (int i = 0; i < 64; i++) buf[i] = 1000 + 50 * i;        /* pure trend */
    dsp_baseline_remove(buf, 64);
    uint32_t pk = dsp_peak_abs(buf, 64);
    CHECK(pk < 20, "baseline removal leaves %u", (unsigned)pk);

    q31_t in[4] = {0, 1000, 2000, 3000}, out[7];
    dsp_interp_linear(in, 4, out, 7);
    CHECK(out[1] == 500 && out[6] == 3000, "linear interpolation (%ld, %ld)", (long)out[1], (long)out[6]);
}

static void test_fft(void)
{
    printf("FFT %u-point Q15\n", AUDIO_FFT_SIZE);
    static cq15_t x[AUDIO_FFT_SIZE];
    for (uint32_t i = 0; i < AUDIO_FFT_SIZE; i++) {
        x[i].re = (int16_t)(16000.0 * cos(2 * M_PI * 10 * i / AUDIO_FFT_SIZE));
        x[i].im = 0;
    }
    dsp_fft_q15(x, AUDIO_FFT_LOG2);
    uint32_t best = 0, bi = 0, other = 0;
    for (uint32_t k = 1; k < AUDIO_FFT_SIZE / 2; k++) {
        uint32_t m = dsp_cmag_sq(x[k]);
        if (m > best) { best = m; bi = k; }
    }
    for (uint32_t k = 1; k < AUDIO_FFT_SIZE / 2; k++) {
        if (k != bi && dsp_cmag_sq(x[k]) > other) other = dsp_cmag_sq(x[k]);
    }
    double mag = sqrt((double)best);
    CHECK(bi == 10, "peak at bin %u", (unsigned)bi);
    CHECK(fabs(mag - 8000.0) < 80.0, "peak magnitude %.1f (expected 8000 = A/2)", mag);
    CHECK(other < best / 10000, "leakage %.1f dB below peak", 10 * log10((double)best / (other + 1)));
}

static double tone_gain(dsp_biquad_coef_t *c, double f)
{
    dsp_biquad_t bq = {.c = *c};
    static q31_t buf[AUDIO_HOP_SIZE];
    double acc_in = 0, acc_out = 0;
    int n = 0;
    for (int blk = 0; blk < 500; blk++) {
        for (int i = 0; i < AUDIO_HOP_SIZE; i++, n++) buf[i] = to_q31(0.5 * sin(2 * M_PI * f * n / SAMPLE_RATE));
        double e_in = 0;
        for (int i = 0; i < AUDIO_HOP_SIZE; i++) e_in += pow(buf[i] / 2147483648.0, 2);
        dsp_biquad_process(&bq, buf, AUDIO_HOP_SIZE);
        if (blk >= 250) {
            acc_in += e_in;
            for (int i = 0; i < AUDIO_HOP_SIZE; i++) acc_out += pow(buf[i] / 2147483648.0, 2);
        }
    }
    return 10 * log10(acc_out / acc_in);
}

static void test_biquad(void)
{
    printf("Biquad filters (Q2.30)\n");
    dsp_biquad_coef_t c;
    dsp_biquad_design(&c, DSP_BQ_LOWPASS, SAMPLE_RATE, 150.0f, 0.707f, 0.0f);
    double g1 = tone_gain(&c, 50), g2 = tone_gain(&c, 150), g3 = tone_gain(&c, 2000);
    CHECK(fabs(g1) < 0.5, "LPF150 @50 Hz: %.2f dB", g1);
    CHECK(fabs(g2 + 3.0) < 0.5, "LPF150 @150 Hz: %.2f dB (expected -3)", g2);
    CHECK(g3 < -40.0, "LPF150 @2 kHz: %.2f dB", g3);
    dsp_biquad_design(&c, DSP_BQ_HIGHPASS, SAMPLE_RATE, 45.0f, 0.707f, 0.0f);
    g1 = tone_gain(&c, 20);
    g2 = tone_gain(&c, 1000);
    CHECK(g1 < -12.0 && fabs(g2) < 0.3, "HPF45: 20 Hz %.2f dB, 1 kHz %.2f dB", g1, g2);
    dsp_biquad_design(&c, DSP_BQ_PEAK, SAMPLE_RATE, 1000.0f, 1.0f, 6.0f);
    g1 = tone_gain(&c, 1000);
    CHECK(fabs(g1 - 6.0) < 0.3, "peak +6 dB @1 kHz: %.2f dB", g1);
}

static void test_envelope_agc(void)
{
    printf("Envelope + AGC\n");
    float pe[STAGE_MAX_PARAMS], pa[STAGE_MAX_PARAMS];
    stage_defaults(&g_stage_envelope, pe);
    stage_defaults(&g_stage_agc, pa);
    stage_start(&g_stage_envelope, pe);
    stage_start(&g_stage_agc, pa);
    audio_frame_t f;
    frame_init(&f);
    int n = 0;
    uint16_t lvl_loud = 0, lvl_quiet = 0, lvl_silence = 0;
    double env_db_loud = 0;
    /* 2 s loud (-12 dBFS), 1 s at -36 dBFS, 2 s silence (noise -90 dBFS) */
    for (int blk = 0; blk < 1250; blk++) {
        double a = blk < 500 ? 0.25 : (blk < 750 ? 0.0158 : 0.0);
        for (int i = 0; i < AUDIO_HOP_SIZE; i++, n++) {
            f.samples[i] = to_q31(a * sin(2 * M_PI * 440 * n / SAMPLE_RATE) + 3e-5 * frand());
        }
        g_stage_envelope.process(g_stage_envelope.state, &f);
        g_stage_agc.process(g_stage_agc.state, &f);
        if (blk == 499) { lvl_loud = f.level; env_db_loud = f.envelope_db / 256.0; }
        if (blk == 749) lvl_quiet = f.level;
        if (blk == 1249) lvl_silence = f.level;
    }
    CHECK(fabs(env_db_loud - (-15.05)) < 1.0, "envelope of -12 dBFS-peak sine (rms -15 dBFS): %.2f dB", env_db_loud);
    CHECK(lvl_loud > 25000, "level loud = %u", lvl_loud);
    CHECK(lvl_quiet < 12000, "level 24 dB quieter = %u", lvl_quiet);
    CHECK(lvl_silence == 0, "level in silence (gated) = %u", lvl_silence);
}

static void test_spectrum(void)
{
    printf("Spectrum stage\n");
    float p[STAGE_MAX_PARAMS];
    stage_defaults(&g_stage_spectrum, p);
    stage_start(&g_stage_spectrum, p);
    audio_frame_t f;
    frame_init(&f);
    int n = 0;
    for (int blk = 0; blk < 200; blk++) {
        for (int i = 0; i < AUDIO_HOP_SIZE; i++, n++) {
            f.samples[i] = to_q31(0.1 * sin(2 * M_PI * 1000 * n / SAMPLE_RATE) + 1e-4 * frand());
        }
        g_stage_spectrum.process(g_stage_spectrum.state, &f);
    }
    /* expected band: log spacing 60..6000 Hz over 12 bands */
    int exp_band = (int)floor(log(1000.0 / 60.0) / log(6000.0 / 60.0) * AUDIO_NUM_BANDS);
    printf("        bands:");
    for (int b = 0; b < AUDIO_NUM_BANDS; b++) printf(" %5u", f.bands[b]);
    printf("\n");
    CHECK(abs((int)f.dominant_band - exp_band) <= 1, "dominant band %u (expected ~%d)", f.dominant_band, exp_band);
    double cen = f.centroid / 32767.0 * SAMPLE_RATE / 2;
    CHECK(fabs(cen - 1000.0) < 120.0, "centroid %.0f Hz", cen);
}

/* Synthetic music: kick drum (decaying 55 Hz burst) on every beat, hi-hat noise on
 * off-beats, a sustained chord and background noise. */
static int s_drop_every;     /* 0 = none, else omit every Nth kick */

static double synth(int n, double bpm, double *kick_phase_out)
{
    double t = (double)n / SAMPLE_RATE;
    double period = 60.0 / bpm;
    double tb = fmod(t, period);
    double th = fmod(t + period / 2, period);
    double kick = 0.55 * exp(-tb / 0.07) * sin(2 * M_PI * (55 + 60 * exp(-tb / 0.02)) * tb);
    if (s_drop_every && ((int)(t / period) % s_drop_every) == s_drop_every - 1) kick = 0.0;
    double hat = (th < 0.03) ? 0.06 * frand() * exp(-th / 0.01) : 0.0;
    double chord = 0.05 * (sin(2 * M_PI * 220 * t) + sin(2 * M_PI * 277 * t) + sin(2 * M_PI * 330 * t));
    if (kick_phase_out) *kick_phase_out = tb;
    return kick + hat + chord + 0.002 * frand();
}

static void run_beat(double bpm, int seconds)
{
    float p[STAGE_MAX_PARAMS], pr[STAGE_MAX_PARAMS];
    stage_defaults(&g_stage_beat, p);
    stage_defaults(&g_stage_render_beat_flash, pr);
    stage_start(&g_stage_beat, p);
    stage_start(&g_stage_render_beat_flash, pr);
    audio_frame_t f;
    frame_init(&f);
    int n = 0, blocks = seconds * SAMPLE_RATE / AUDIO_HOP_SIZE;
    int beats_late = 0, beats_aligned = 0;
    double delay_sum = 0;
    double period = 60.0 / bpm;
    for (int blk = 0; blk < blocks; blk++) {
        double ph = 0;
        for (int i = 0; i < AUDIO_HOP_SIZE; i++, n++) f.samples[i] = to_q31(synth(n, bpm, &ph));
        g_stage_beat.process(g_stage_beat.state, &f);
        g_stage_render_beat_flash.process(g_stage_render_beat_flash.state, &f);
        if (getenv("BEAT_DEBUG") && (f.beat || f.onset > 600)) {
            printf("      t=%6.3f %s onset=%5.2fdB kick_ph=%5.3f bpm=%5.1f str=%u\n", (double)n / SAMPLE_RATE,
                   f.beat ? "BEAT" : "    ", f.onset / 256.0, ph, f.bpm_x10 / 10.0, f.beat_strength);
        }
        if (blk >= blocks / 2 && f.beat) {
            beats_late++;
            /* distance of this beat to the nearest kick onset */
            double d = fmin(ph, period - ph);
            if (d < 0.040) beats_aligned++;
            if (ph < period / 2) delay_sum += ph;
        }
    }
    double expected = (seconds / 2.0) / period;
    double bpm_est = f.bpm_x10 / 10.0;
    CHECK(fabs(bpm_est - bpm) < bpm * 0.04, "%.0f BPM: tempo estimate %.1f BPM", bpm, bpm_est);
    CHECK(fabs(beats_late - expected) <= 2, "%.0f BPM: %d beats in last %d s (expected %.0f)", bpm, beats_late,
          seconds / 2, expected);
    CHECK(beats_aligned >= beats_late - 1, "%.0f BPM: %d/%d beats within 40 ms of the kick (mean delay %.1f ms)", bpm,
          beats_aligned, beats_late, beats_late ? 1000.0 * delay_sum / beats_late : 0.0);
}

static void test_beat(void)
{
    printf("Beat detector (synthetic kick pattern)\n");
    run_beat(120.0, 16);
    run_beat(128.0, 16);
    run_beat(95.0, 16);
    run_beat(140.0, 16);
    printf("  (every 4th kick missing -> flywheel)\n");
    s_drop_every = 4;
    run_beat(120.0, 16);
    s_drop_every = 0;
}

static void test_renderers(void)
{
    printf("Render stages\n");
    float p[STAGE_MAX_PARAMS];
    audio_frame_t f;
    frame_init(&f);

    stage_defaults(&g_stage_render_spectrum, p);
    stage_start(&g_stage_render_spectrum, p);
    memset(f.bands, 0, sizeof(f.bands));
    f.bands[0] = 32767;         /* only the lowest band full */
    g_stage_render_spectrum.process(g_stage_render_spectrum.state, &f);
    int seg = LED_MAX_COUNT / AUDIO_NUM_BANDS;
    int lit_first = 0, lit_rest = 0;
    for (int i = 0; i < LED_MAX_COUNT; i++) {
        bool on = (s_leds[i].r | s_leds[i].g | s_leds[i].b) != 0;
        if (i < seg) lit_first += on; else lit_rest += on;
    }
    CHECK(lit_first == seg && lit_rest == 0, "spectrum render: band 0 -> first %d LEDs (%d other)", lit_first, lit_rest);

    stage_defaults(&g_stage_render_beat_segments, p);
    stage_start(&g_stage_render_beat_segments, p);
    memset(&f.bands, 0, sizeof(f.bands));
    f.beat = true;
    f.beat_strength = 16384;
    f.centroid = 1500;          /* low centroid -> left end */
    g_stage_render_beat_segments.process(g_stage_render_beat_segments.state, &f);
    int first = -1, last = -1;
    for (int i = 0; i < LED_MAX_COUNT; i++) {
        if (s_leds[i].r | s_leds[i].g | s_leds[i].b) { if (first < 0) first = i; last = i; }
    }
    CHECK(first == 0 && last > 0 && last < 15, "beat segments: bassy beat lights LEDs %d..%d", first, last);
}

/* ========================================================================== */
/*  Music show: melody, music analysis, render show                          */
/* ========================================================================== */

static const stage_def_t *const s_show_chain[] = {
    &g_stage_envelope, &g_stage_agc, &g_stage_spectrum, &g_stage_beat,
    &g_stage_melody, &g_stage_music, &g_stage_render_show,
};
#define SHOW_N  ((int)(sizeof(s_show_chain) / sizeof(s_show_chain[0])))

static float s_show_genre = GENRE_AUTO;

static void show_start(void)
{
    for (int i = 0; i < SHOW_N; i++) {
        float p[STAGE_MAX_PARAMS];
        stage_defaults(s_show_chain[i], p);
        if (s_show_chain[i] == &g_stage_music) p[MUSIC_P_GENRE] = s_show_genre;
        stage_start(s_show_chain[i], p);
    }
}

static void show_block(audio_frame_t *f)
{
    f->beat = false;
    f->beat_strength = 0;
    f->spectrum_valid = false;
    for (int i = 0; i < SHOW_N; i++) s_show_chain[i]->process(s_show_chain[i]->state, f);
}

static int lit_count(void)
{
    int lit = 0;
    for (int i = 0; i < LED_MAX_COUNT; i++) lit += (s_leds[i].r | s_leds[i].g | s_leds[i].b) > 8;
    return lit;
}

static const char *const s_scene_names[SCENE_COUNT] = {"silence", "ambient", "groove", "build", "drop", "fill", "lead"};

/* SHOW_DEBUG=1 prints the analysis every 100 ms */
static void show_trace(const audio_frame_t *f, double t)
{
    if (!getenv("SHOW_DEBUG")) return;
    const music_t *m = &f->music;
    printf("      t=%6.2f %-7s g=%.2f l=%.2f p=%.2f a=%.2f clar=%.2f int=%.2f bld=%.2f bpm=%5.1f conf=%.2f "
           "sal=%.2f %s pitch=%5.1f hits=%02x %s lit=%d\n",
           t, s_scene_names[m->scene], m->w_groove / 32767.0, m->w_lead / 32767.0, m->w_perc / 32767.0,
           m->w_ambient / 32767.0, m->clarity / 32767.0, m->intensity / 32767.0, m->build / 32767.0,
           f->bpm_x10 / 10.0, f->tempo_conf / 32767.0, f->melody.salience / 32767.0, f->melody.active ? "ON " : "off",
           f->melody.pitch_q8 / 256.0, m->hits, music_profile(m->genre)->name, lit_count());
}

/* A plucked / sung note with harmonics. */
static double note_wave(double t, double midi, double amp)
{
    double f0 = 440.0 * pow(2.0, (midi - 69.0) / 12.0);
    double v = 0;
    for (int h = 1; h <= 4; h++) v += sin(2 * M_PI * f0 * h * t) / (h * h);
    return amp * v;
}

static const double s_tune[] = {69, 72, 76, 74, 71, 67, 72, 79};    /* lead line, 220 ms per note */
#define NOTE_LEN    0.22

/* legato line: each note fades in over 10 ms while the previous one fades out over 20 ms */
static double lead_line(double t, int *note_idx)
{
    int k = (int)(t / NOTE_LEN);
    double tn = t - k * NOTE_LEN;
    if (note_idx) *note_idx = k;
    double env = (tn < 0.01 ? tn / 0.01 : 1.0) * exp(-tn / 0.6);
    double v = note_wave(t, s_tune[k % 8], 0.25 * env);
    if (k > 0 && tn < 0.02) v += note_wave(t, s_tune[(k - 1) % 8], 0.25 * exp(-NOTE_LEN / 0.6) * (1.0 - tn / 0.02));
    return v;
}

static double tom_hit(double tb, double f0)
{
    return 0.5 * exp(-tb / 0.08) * sin(2 * M_PI * f0 * (1 + 0.3 * exp(-tb / 0.02)) * tb) +
           0.08 * frand() * exp(-tb / 0.02);
}

static void test_melody(void)
{
    printf("Melody tracker (harmonic notes)\n");
    float p[STAGE_MAX_PARAMS];
    stage_defaults(&g_stage_melody, p);
    stage_start(&g_stage_melody, p);
    audio_frame_t f;
    frame_init(&f);
    int n = 0, notes = 0, doubles = 0, last_note = -1, active_blocks = 0, blocks = 0;
    double max_err = 0;
    for (int blk = 0; blk < 8 * 55; blk++) {                       /* 8 notes */
        int idx = 0;
        for (int i = 0; i < AUDIO_HOP_SIZE; i++, n++) {
            f.samples[i] = to_q31(lead_line((double)n / SAMPLE_RATE, &idx) + 1e-4 * frand());
        }
        g_stage_melody.process(g_stage_melody.state, &f);
        double tn = (double)n / SAMPLE_RATE - idx * NOTE_LEN;
        if (f.melody.note_on) {
            notes++;
            if (idx == last_note) doubles++;                       /* two starts in one note */
            last_note = idx;
        }
        if (tn > 0.08 && tn < 0.2) {
            blocks++;
            if (f.melody.active) {
                active_blocks++;
                double e = fabs(f.melody.pitch_q8 / 256.0 - s_tune[idx % 8]);
                if (e > max_err) max_err = e;
            }
        }
    }
    CHECK(notes >= 7 && notes <= 9 && doubles == 0, "%d note starts for 8 notes (%d double)", notes, doubles);
    CHECK(active_blocks >= blocks * 9 / 10, "voice active in %d/%d mid-note blocks", active_blocks, blocks);
    CHECK(max_err < 0.5, "pitch error max %.2f semitones", max_err);

    /* drums only: no voice */
    stage_start(&g_stage_melody, p);
    int act = 0;
    n = 0;
    for (int blk = 0; blk < 1000; blk++) {
        for (int i = 0; i < AUDIO_HOP_SIZE; i++, n++) {
            double t = (double)n / SAMPLE_RATE;
            double th = fmod(t, 0.125);
            f.samples[i] = to_q31(tom_hit(fmod(t, 0.25), 120) + 0.1 * frand() * exp(-th / 0.02));
        }
        g_stage_melody.process(g_stage_melody.state, &f);
        act += f.melody.active;
    }
    CHECK(act < 100, "drums only: voice active in %d/1000 blocks", act);
}

typedef double (*show_gen_t)(int n);

typedef struct {
    int blocks;
    int scene[SCENE_COUNT];
    int beat_full, beat_partial;        /* LEDs lit two blocks after a beat */
    int max_lit;
    double w_groove, w_lead, w_perc;
} show_stats_t;

static audio_frame_t s_show_f;
static int s_show_n;

static void show_reset(void)
{
    show_start();
    frame_init(&s_show_f);
    s_show_n = 0;
}

/* Continue the show for @p seconds of @p gen; statistics over the part after @p skip seconds. */
static void show_run(show_gen_t gen, double seconds, double skip, show_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    int blocks = (int)(seconds * SAMPLE_RATE / AUDIO_HOP_SIZE);
    int pending = 0;
    for (int blk = 0; blk < blocks; blk++) {
        for (int i = 0; i < AUDIO_HOP_SIZE; i++, s_show_n++) s_show_f.samples[i] = to_q31(gen(s_show_n));
        show_block(&s_show_f);
        if ((blk % 25) == 0) show_trace(&s_show_f, (double)s_show_n / SAMPLE_RATE);
        if (s_show_f.beat && getenv("HIT_DEBUG")) {
            printf("        BEAT t=%6.3f%s str=%.2f bar=%u\n", (double)s_show_n / SAMPLE_RATE,
                   s_show_f.beat_predicted ? " (flywheel)" : "", s_show_f.beat_strength / 32767.0,
                   s_show_f.music.bar_beat);
        }
        if (s_show_f.music.hits && getenv("HIT_DEBUG")) {
            static const char *const hn[HIT_COUNT] = {"kick", "tom", "snare", "hat", "crash"};
            printf("        hit t=%6.3f", (double)s_show_n / SAMPLE_RATE);
            for (int h = 0; h < HIT_COUNT; h++) {
                if (s_show_f.music.hits & (1u << h)) printf(" %s(%.2f)", hn[h], s_show_f.music.hit_strength[h] / 32767.0);
            }
            printf(" sal=%.2f %s\n", s_show_f.melody.salience / 32767.0, s_show_f.melody.active ? "voice" : "");
        }
        if (blk < skip * SAMPLE_RATE / AUDIO_HOP_SIZE) continue;
        const music_t *m = &s_show_f.music;
        st->blocks++;
        st->scene[m->scene]++;
        st->w_groove += m->w_groove / 32767.0;
        st->w_lead += m->w_lead / 32767.0;
        st->w_perc += m->w_perc / 32767.0;
        int lit = lit_count();
        if (lit > st->max_lit) st->max_lit = lit;
        if (pending && --pending == 0) {
            if (lit >= LED_MAX_COUNT - 2) st->beat_full++;
            else if (lit > 0) st->beat_partial++;
        }
        if (s_show_f.beat) pending = 2;
    }
    if (st->blocks) {
        st->w_groove /= st->blocks;
        st->w_lead /= st->blocks;
        st->w_perc /= st->blocks;
    }
}

static double gen_groove(int n) { return synth(n, 128.0, NULL); }

static double gen_lead(int n) { return lead_line((double)n / SAMPLE_RATE, NULL) + 0.002 * frand(); }

/* groove; from 8 to 9 s the kick and hats stop for a fill of descending 16th-note toms */
static double gen_fill(int n)
{
    double t = (double)n / SAMPLE_RATE;
    if (t >= 8.0 && t < 9.0) {
        double sx = 60.0 / 128.0 / 4.0;
        int k = (int)((t - 8.0) / sx);
        static const double toms[] = {220, 180, 150, 120};
        return tom_hit(t - 8.0 - k * sx, toms[(k / 2) % 4]) + 0.002 * frand();
    }
    return synth(n, 128.0, NULL);
}

/* EDM-like: groove, an 8 s build-up with rising noise and a speeding snare roll (no kick),
 * then the drop: the groove comes back with a heavier kick */
static double gen_build_drop(int n)
{
    double t = (double)n / SAMPLE_RATE;
    double beat = 60.0 / 128.0;
    if (t < 8.0) return 0.6 * synth(n, 128.0, NULL);
    if (t < 16.0) {
        double r = (t - 8.0) / 8.0;
        double tb = fmod(t, beat / (r < 0.5 ? 2.0 : 4.0));
        double snare = 0.15 * r * frand() * exp(-tb / 0.03);
        double chord = 0.03 * (sin(2 * M_PI * 220 * t) + sin(2 * M_PI * 330 * t));
        return 0.12 * r * r * frand() + snare + chord;
    }
    double tk = fmod(t, beat);
    return synth(n, 128.0, NULL) + 0.4 * exp(-tk / 0.07) * sin(2 * M_PI * 50 * tk);
}

static double gen_silence(int n) { (void)n; return 0.0; }

/* Trap: 808 (long 50 Hz tail) every 2 beats at 140 BPM, clap on 3, 16th hats with a
 * 32nd roll on the last beat of the bar */
static double gen_trap(int n)
{
    double t = (double)n / SAMPLE_RATE;
    double beat = 60.0 / 140.0;
    double tk = fmod(t, 2 * beat);
    double k808 = 0.6 * exp(-tk / 0.45) * sin(2 * M_PI * (50 + 30 * exp(-tk / 0.02)) * tk);
    double tc = fmod(t + 2 * beat, 4 * beat);
    double clap = tc < 0.05 ? 0.25 * frand() * exp(-tc / 0.015) : 0.0;
    int step16 = (int)(fmod(t, 4 * beat) / (beat / 4));
    double th = fmod(t, step16 >= 12 ? beat / 8 : beat / 4);
    static double prev;                                            /* hats: high-passed noise */
    double nz = frand(), hp = nz - prev;
    prev = nz;
    double hat = 0.06 * hp * exp(-th / 0.008);
    return k808 + clap + hat + 0.002 * frand();
}

static void test_music_show(void)
{
    printf("Music show (scenes, all/some LEDs, genre)\n");
    show_stats_t st;
    s_show_genre = GENRE_AUTO;

    show_reset();
    show_run(gen_groove, 16.0, 6.0, &st);
    CHECK(st.scene[SCENE_GROOVE] > st.blocks * 8 / 10, "groove: GROOVE scene %d/%d blocks (w_groove %.2f)",
          st.scene[SCENE_GROOVE], st.blocks, st.w_groove);
    CHECK(st.beat_full > 0 && st.beat_partial > 0, "groove: beats light all LEDs %d x and some LEDs %d x",
          st.beat_full, st.beat_partial);

    show_reset();
    show_run(gen_lead, 6.0, 1.5, &st);
    CHECK(st.scene[SCENE_LEAD] > st.blocks * 8 / 10 && st.w_groove < 0.2,
          "lead only: LEAD scene %d/%d blocks, w_groove %.2f, w_lead %.2f", st.scene[SCENE_LEAD], st.blocks,
          st.w_groove, st.w_lead);
    CHECK(st.max_lit <= 12, "lead only: at most %d LEDs lit (sparks, not the whole strip)", st.max_lit);

    show_reset();
    show_run(gen_fill, 8.3, 0.0, &st);
    show_stats_t fill;
    show_run(gen_fill, 0.7, 0.0, &fill);
    CHECK(fill.scene[SCENE_FILL] > fill.blocks * 7 / 10, "fill: FILL scene %d/%d blocks from 0.3 s into the fill",
          fill.scene[SCENE_FILL], fill.blocks);
    show_run(gen_fill, 4.0, 2.0, &st);
    CHECK(st.scene[SCENE_GROOVE] > st.blocks * 8 / 10, "fill: back to GROOVE after the fill (%d/%d)",
          st.scene[SCENE_GROOVE], st.blocks);

    show_reset();
    show_run(gen_build_drop, 16.0, 13.0, &st);
    CHECK(st.scene[SCENE_BUILD] > st.blocks / 2, "build: BUILD scene %d/%d blocks in the last 3 s of an 8 s build-up",
          st.scene[SCENE_BUILD], st.blocks);
    show_run(gen_build_drop, 3.0, 0.2, &st);
    CHECK(st.scene[SCENE_DROP] > st.blocks * 7 / 10, "drop: DROP scene %d/%d blocks after the bass returns",
          st.scene[SCENE_DROP], st.blocks);

    show_reset();
    show_run(gen_silence, 3.0, 1.0, &st);
    CHECK(st.max_lit == 0 && st.scene[SCENE_SILENCE] == st.blocks, "silence: dark (%d lit), SILENCE scene",
          st.max_lit);

    show_reset();
    show_run(gen_groove, 30.0, 29.0, &st);
    CHECK(s_show_f.music.genre == GENRE_EDM, "auto genre: 128 BPM four-on-the-floor -> %s",
          music_profile(s_show_f.music.genre)->name);
    show_reset();
    show_run(gen_trap, 30.0, 29.0, &st);
    CHECK(s_show_f.music.genre == GENRE_TRAP, "auto genre: 808 + hi-hat rolls -> %s",
          music_profile(s_show_f.music.genre)->name);

    s_show_genre = GENRE_ROCK;
    show_reset();
    show_run(gen_groove, 2.0, 1.0, &st);
    CHECK(s_show_f.music.genre == GENRE_ROCK, "fixed genre parameter is used (%s)",
          music_profile(s_show_f.music.genre)->name);
    s_show_genre = GENRE_AUTO;
}

int main(void)
{
    srand(1234);
    dsp_init();
    test_crc();
    test_math();
    test_fft();
    test_biquad();
    test_envelope_agc();
    test_spectrum();
    test_beat();
    test_renderers();
    test_melody();
    test_music_show();
    test_modules_run(&s_checks, &s_fails);
    printf("\n%d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
