/**
 * @file test_host.c
 * @brief Host (PC) sanity tests of the DSP library and the processing stages.
 *
 * Built with the native compiler by Scripts/run_host_tests.sh; feeds synthetic audio
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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int s_fails;
static int s_checks;

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

    stage_defaults(&g_stage_render_level, p);
    stage_start(&g_stage_render_level, p);
    f.level = 30000;
    g_stage_render_level.process(g_stage_render_level.state, &f);
    int lit = 0;
    for (int i = 0; i < LED_MAX_COUNT; i++) lit += (s_leds[i].r | s_leds[i].g | s_leds[i].b) != 0;
    CHECK(lit == LED_MAX_COUNT, "level render: loud -> all %d LEDs on", lit);
    f.level = 1000;
    g_stage_render_level.process(g_stage_render_level.state, &f);   /* hold time */
    for (int k = 0; k < 20; k++) g_stage_render_level.process(g_stage_render_level.state, &f);
    lit = 0;
    for (int i = 0; i < LED_MAX_COUNT; i++) lit += (s_leds[i].r | s_leds[i].g | s_leds[i].b) != 0;
    CHECK(lit == 0, "level render: quiet -> all off (%d lit)", lit);

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
    printf("\n%d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
