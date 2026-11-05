/**
 * @file spectrum_taps.c
 * @brief Signal taps for the host's live spectrum view (see spectrum_taps.h).
 *
 * Hand-off without locks: the real-time task (highest priority, never blocks while
 * writing) bumps a sequence counter before and after each block it writes; the comm task
 * copies the ring and retries if the counter changed meanwhile. The FFT uses the same
 * window, block floating point and dB reference as stage_spectrum.
 */
#include "spectrum_taps.h"

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define N               AUDIO_FFT_SIZE
/* Power of a full-scale sine after window+FFT, dB, as in stage_spectrum (which sums the
 * Hann main lobe: centre bin + 2 neighbours = 1.5 x the centre bin). A point here is often
 * a single bin, so the reference is 10*log10(1.5) = 1.76 dB lower: a sine reads its level. */
#define FS_REF_Q8       (80 * 256 - 451)
#define SPEC_FLOOR_DB   (-120)              /* = LIVE_SPEC_FLOOR_DB of the protocol */
#define STALE_TICKS     pdMS_TO_TICKS(50)

typedef struct {
    int16_t           ring[N];
    uint16_t          pos;
    volatile uint32_t seq;                  /* odd while the real-time task writes */
    volatile uint32_t count;                /* samples written since the slot was assigned */
    volatile TickType_t stamp;              /* tick of the last block */
} tap_ring_t;

static tap_ring_t        s_ring[TAP_MAX_SLOTS];
static volatile int8_t   s_slot_of[TAP_END + 1];
static volatile uint16_t s_mask;
static uint8_t           s_edges[TAP_POINTS + 1];
static bool              s_edges_ok;

/* comm task only */
static q15_t  s_win[N];
static cq15_t s_buf[N];
static int16_t s_snap[N];
static bool   s_win_ok;

static void init_edges(void)
{
    /* log spacing over bins 1..N/2 with at least one bin per point: all bins are kept at
     * the low end, only the top octave(s) are grouped */
    s_edges[0] = 1;
    for (uint32_t i = 1; i <= TAP_POINTS; i++) {
        int k = (int)lroundf(powf((float)(N / 2 + 1), (float)i / TAP_POINTS));
        if (k <= s_edges[i - 1]) k = s_edges[i - 1] + 1;
        int max_k = (int)(N / 2 + 1) - (int)(TAP_POINTS - i);
        if (k > max_k) k = max_k;
        s_edges[i] = (uint8_t)k;
    }
    s_edges_ok = true;
}

const uint8_t *taps_edges(void)
{
    if (!s_edges_ok) init_edges();
    return s_edges;
}

uint16_t taps_set(uint16_t mask)
{
    s_mask = 0;                             /* real-time task stops capturing first */
    __asm__ volatile("" ::: "memory");
    uint16_t kept = 0;
    uint8_t slot = 0;
    for (uint8_t t = 0; t <= TAP_END; t++) {
        s_slot_of[t] = -1;
        if ((mask & (1u << t)) && slot < TAP_MAX_SLOTS && (t == TAP_END || t < STAGE_COUNT)) {
            tap_ring_t *r = &s_ring[slot];
            r->pos = 0;
            r->count = 0;
            s_slot_of[t] = (int8_t)slot++;
            kept |= (uint16_t)(1u << t);
        }
    }
    __asm__ volatile("" ::: "memory");
    s_mask = kept;
    return kept;
}

uint16_t taps_mask(void)
{
    return s_mask;
}

void taps_capture(uint8_t tap, const q31_t *x, uint16_t n)
{
    int8_t slot = s_slot_of[tap];
    if (slot < 0) return;
    tap_ring_t *r = &s_ring[slot];
    r->seq++;
    __asm__ volatile("" ::: "memory");
    uint16_t pos = r->pos;
    for (uint16_t i = 0; i < n; i++) {
        r->ring[pos] = (int16_t)(x[i] >> 16);
        pos = (uint16_t)((pos + 1u) & (N - 1u));
    }
    r->pos = pos;
    r->count += n;
    r->stamp = xTaskGetTickCount();
    __asm__ volatile("" ::: "memory");
    r->seq++;
}

bool taps_spectrum(uint8_t tap, uint8_t *out)
{
    if (tap > TAP_END || !(s_mask & (1u << tap))) return false;
    int8_t slot = s_slot_of[tap];
    if (slot < 0) return false;
    tap_ring_t *r = &s_ring[slot];

    /* consistent copy of the newest N samples, oldest first */
    bool ok = false;
    for (int attempt = 0; attempt < 4 && !ok; attempt++) {
        uint32_t s1 = r->seq;
        if (s1 & 1u) continue;
        __asm__ volatile("" ::: "memory");
        uint16_t pos = r->pos;
        for (uint32_t i = 0; i < N; i++) s_snap[i] = r->ring[(pos + i) & (N - 1u)];
        __asm__ volatile("" ::: "memory");
        ok = (r->seq == s1);
    }
    if (!ok || r->count < N || (xTaskGetTickCount() - r->stamp) > STALE_TICKS) return false;

    if (!s_win_ok) {
        dsp_window_hann_q15(s_win, N);
        s_win_ok = true;
    }
    const uint8_t *e = taps_edges();

    /* block floating point, as in stage_spectrum */
    int32_t mx = 1;
    for (uint32_t i = 0; i < N; i++) {
        int32_t a = s_snap[i] < 0 ? -s_snap[i] : s_snap[i];
        if (a > mx) mx = a;
    }
    int shift = __builtin_clz((uint32_t)mx) - 17;
    if (shift < 0) shift = 0;
    if (shift > 12) shift = 12;
    for (uint32_t i = 0; i < N; i++) {
        int32_t v = (int32_t)s_snap[i] << shift;
        s_buf[i].re = (int16_t)((v * s_win[i]) >> 15);
        s_buf[i].im = 0;
    }
    dsp_fft_q15(s_buf, AUDIO_FFT_LOG2);

    const int32_t comp_q8 = FS_REF_Q8 + 1541 * shift;
    for (uint32_t p = 0; p < TAP_POINTS; p++) {
        uint64_t pw = 0;
        for (uint32_t k = e[p]; k < e[p + 1]; k++) {
            pw += dsp_cmag_sq(s_buf[k]);              /* k <= N/2 (Nyquist) */
        }
        pw /= (uint32_t)(e[p + 1] - e[p]);          /* mean power: a tone and noise read alike */
        int32_t db_q8 = dsp_power_to_db_q8(pw) - comp_q8;
        int32_t v = (db_q8 - SPEC_FLOOR_DB * 256) / 128;    /* 0.5 dB steps */
        out[p] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    return true;
}
