/**
 * @file stage_render_show.c
 * @brief Render: scene-aware light show. Four layers, mixed by the weights of the
 *        music analysis stage (audio_frame_t.music):
 *
 * - Groove: on every beat either ALL LEDs (downbeat, strong beat, drop, loud section)
 *   or SOME LEDs (halves, centre burst, segments, chase; rotated per bar). Snare
 *   accents, drop strobe and the 808 / sub-bass glow depend on the genre profile.
 * - Lead: every note of the lead voice is a spark; position = pitch, hue = pitch
 *   class, brightness follows the note, bends glide, released notes leave a trail.
 * - Percussion: toms = flashes placed by drum pitch (rolls sweep), snare = centre
 *   burst, kick = pulses at both ends, hi-hats = twinkles (runs during rolls),
 *   crash = shimmer over the whole strip.
 * - Ambient: slow breathing gradient following the level; build-ups grow bars from
 *   both ends towards the centre.
 */
#include "audio_types.h"
#include "led_color.h"
#include "music_profiles.h"

#include <math.h>
#include <string.h>

#define MAX_SPARKS  8
#define MAX_PULSES  8
#define BPAT_FULL    0xFF
#define Q15F(x)     ((int32_t)((x) * 32767))

typedef struct {
    int32_t  pos_q8, target_q8;
    int32_t  amp;                   /* Q15 */
    uint16_t hue;
    bool     held;                  /* still follows the sounding note */
} spark_t;

typedef struct {
    int32_t  center_q8, half_q8;
    int32_t  amp;                   /* Q15 */
    int32_t  decay;                 /* Q15 per block */
    uint16_t hue;
    uint8_t  sat;
} pulse_t;

typedef struct {
    /* config */
    bool     custom;
    uint16_t hue;
    uint16_t sat_q8;
    int32_t  full_bias;             /* Q15 */
    uint16_t on_override;           /* blocks, 0 = profile */
    int32_t  trail;                 /* Q15 per block */
    uint16_t sparkle;               /* Q8 */
    uint16_t ambient;               /* 0..256 */
    uint8_t  max_sparks;
    int32_t  dec_profile[GENRE_COUNT];
    uint16_t on_profile[GENRE_COUNT];
    int32_t  dec_kick, dec_tom, dec_snare, dec_crash, dec_accent;
    /* groove */
    uint8_t  pat;
    bool     flip;
    uint8_t  amp;
    int32_t  env;                   /* Q15 */
    uint16_t on_cnt;
    int32_t  width;                 /* LEDs, centre pattern */
    uint16_t g_hue;
    bool     white;
    bool     was_drop;
    int32_t  avg_str;
    int32_t  accent;                /* Q15 */
    /* lead */
    spark_t  sp[MAX_SPARKS];
    uint8_t  sp_next;
    int32_t  pmin, pmax;            /* pitch range, Q8 semitones */
    /* percussion */
    pulse_t  pl[MAX_PULSES];
    uint8_t  pl_next;
    uint8_t  tw[LED_MAX_COUNT];
    int32_t  crash;                 /* Q15 */
    uint32_t rnd;
    uint16_t run_pos;
    /* ambient */
    uint16_t phase;
    led_rgb_t layer[LED_MAX_COUNT];
} rshow_state_t;

static rshow_state_t s_st;

static const param_desc_t s_params[RSHOW_P_COUNT] = {
    [RSHOW_P_PALETTE]       = PDESC_E("palette", "genre|custom", 2, 0.0f),
    [RSHOW_P_HUE]           = PDESC_I("hue", "deg", 0.0f, 359.0f, 280.0f),
    [RSHOW_P_SATURATION]    = PDESC_F("saturation", "", 0.0f, 1.0f, 1.0f, 0.01f),
    [RSHOW_P_FULL_BIAS]     = PDESC_F("full_bias", "", -1.0f, 1.0f, 0.0f, 0.05f),
    [RSHOW_P_ON_TIME_MS]    = PDESC_I("on_time", "ms", 0.0f, 1000.0f, 0.0f),
    [RSHOW_P_LEAD_TRAIL_MS] = PDESC_I("lead_trail", "ms", 20.0f, 3000.0f, 300.0f),
    [RSHOW_P_SPARKLE]       = PDESC_F("sparkle", "x", 0.0f, 3.0f, 1.0f, 0.05f),
    [RSHOW_P_AMBIENT]       = PDESC_F("ambient", "", 0.0f, 1.0f, 0.3f, 0.01f),
    [RSHOW_P_MAX_SPARKS]    = PDESC_I("max_sparks", "", 1.0f, (float)MAX_SPARKS, 6.0f),
};

static int32_t decay_q15(float ms)
{
    return (int32_t)(expf(-BLOCK_MS / ms) * 32767.0f);
}

static int32_t clamp15(int32_t v)
{
    return v < 0 ? 0 : (v > 32767 ? 32767 : v);
}

static void reset(void *st)
{
    rshow_state_t *s = st;
    s->pat = BPAT_FULL;
    s->flip = false;
    s->amp = 0;
    s->env = 0;
    s->on_cnt = 0;
    s->width = 0;
    s->g_hue = 0;
    s->white = false;
    s->was_drop = false;
    s->avg_str = 16384;
    s->accent = 0;
    memset(s->sp, 0, sizeof(s->sp));
    s->sp_next = 0;
    s->pmin = 52 * 256;             /* E3 .. E6, widened by the music */
    s->pmax = 88 * 256;
    memset(s->pl, 0, sizeof(s->pl));
    s->pl_next = 0;
    memset(s->tw, 0, sizeof(s->tw));
    s->crash = 0;
    s->rnd = 0x2545F491u;
    s->run_pos = 0;
    s->phase = 0;
}

static void configure(void *st, const float *p)
{
    rshow_state_t *s = st;
    s->custom = p[RSHOW_P_PALETTE] > 0.5f;
    s->hue = (uint16_t)p[RSHOW_P_HUE];
    s->sat_q8 = (uint16_t)(p[RSHOW_P_SATURATION] * 256.0f);
    s->full_bias = (int32_t)(p[RSHOW_P_FULL_BIAS] * 32767.0f);
    s->on_override = (uint16_t)(p[RSHOW_P_ON_TIME_MS] / BLOCK_MS + 0.5f);
    s->trail = decay_q15(p[RSHOW_P_LEAD_TRAIL_MS]);
    s->sparkle = (uint16_t)(p[RSHOW_P_SPARKLE] * 256.0f);
    s->ambient = (uint16_t)(p[RSHOW_P_AMBIENT] * 256.0f);
    s->max_sparks = (uint8_t)p[RSHOW_P_MAX_SPARKS];
    if (s->max_sparks < 1) s->max_sparks = 1;
    if (s->max_sparks > MAX_SPARKS) s->max_sparks = MAX_SPARKS;
    for (uint8_t g = 0; g < GENRE_COUNT; g++) {
        const music_profile_t *pr = music_profile(g);
        s->dec_profile[g] = decay_q15((float)pr->decay_ms);
        s->on_profile[g] = (uint16_t)((float)pr->on_ms / BLOCK_MS + 0.5f);
    }
    s->dec_kick = decay_q15(100.0f);
    s->dec_tom = decay_q15(120.0f);
    s->dec_snare = decay_q15(150.0f);
    s->dec_crash = decay_q15(600.0f);
    s->dec_accent = decay_q15(150.0f);
}

/* layer += scale(px), with the layer weight already applied by the caller */
static void composite(led_rgb_t *out, const led_rgb_t *layer, int32_t n, int32_t w_q15)
{
    uint8_t w = (uint8_t)(clamp15(w_q15) >> 7);
    if (w == 0) return;
    for (int32_t i = 0; i < n; i++) out[i] = led_add(out[i], led_scale(layer[i], w));
}

static void draw_tri(led_rgb_t *px, int32_t n, int32_t center_q8, int32_t half_q8, uint16_t hue, uint8_t sat,
                     uint8_t amp)
{
    int32_t hw = half_q8 > 128 ? half_q8 : 128;
    int32_t lo = (center_q8 - hw) >> 8, hi = (center_q8 + hw) >> 8;
    if (lo < 0) lo = 0;
    if (hi > n - 1) hi = n - 1;
    for (int32_t i = lo; i <= hi; i++) {
        int32_t d = i * 256 - center_q8;
        if (d < 0) d = -d;
        if (d >= hw) continue;
        uint8_t v = (uint8_t)(((int32_t)amp * (hw - d)) / hw);
        px[i] = led_add(px[i], led_hsv(hue, sat, led_gamma8(v)));
    }
}

static uint8_t pick_pattern(uint8_t mask, uint32_t bar)
{
    uint8_t cnt = (uint8_t)__builtin_popcount(mask);
    if (cnt == 0) return BPAT_CENTER;
    uint8_t k = (uint8_t)(bar % cnt);
    for (uint8_t p = 0; p < BPAT_COUNT; p++) {
        if ((mask & BPAT_BIT(p)) && k-- == 0) return p;
    }
    return BPAT_CENTER;
}

/* ---- groove ---------------------------------------------------------------- */

static void groove_trigger(rshow_state_t *s, const audio_frame_t *f, const music_profile_t *pr,
                           const uint16_t *pal, uint16_t str, bool force_full)
{
    const music_t *m = &f->music;
    s->avg_str += ((int32_t)str - s->avg_str) / 8;
    int32_t rel = clamp15(((int32_t)str * 16384) / (s->avg_str > 1000 ? s->avg_str : 1000));
    int32_t full = (35 * (int32_t)m->intensity + 25 * rel) / 100 + (m->bar_beat == 0 ? Q15F(0.3) : 0) +
                   (m->drop ? Q15F(0.35) : 0) + pr->full_bias + s->full_bias - (4 * (int32_t)m->w_lead) / 10;
    bool drop_edge = m->drop && !s->was_drop;
    s->pat = (force_full || drop_edge || full >= 16384) ? BPAT_FULL : pick_pattern(pr->patterns, m->bar_count);
    s->amp = (uint8_t)(110u + ((145u * str) >> 15));
    s->env = (int32_t)s->amp << 7;
    s->on_cnt = s->on_override ? s->on_override : s->on_profile[m->genre];
    s->flip = !s->flip;
    s->width = ((int32_t)f->num_leds * (Q15F(0.3) + (Q15F(0.6) * (int32_t)str >> 15))) >> 15;
    s->white = drop_edge && pr->drop_strobe;
    s->g_hue = pal[m->bar_count & 1u];
}

static void groove_draw(rshow_state_t *s, const audio_frame_t *f, const music_profile_t *pr, const uint16_t *pal,
                        uint8_t sat)
{
    const int32_t n = f->num_leds;
    led_rgb_t *px = s->layer;
    led_fill(px, (uint16_t)n, LED_BLACK);

    uint8_t bri;
    if (pr->decay_style && !s->on_override) {
        bri = led_gamma8((uint8_t)(s->env >> 7));
        s->env = (s->env * s->dec_profile[f->music.genre]) >> 15;
    } else {
        bri = s->on_cnt ? s->amp : 0;
        if (s->on_cnt) s->on_cnt--;
    }
    if (bri) {
        led_rgb_t c = led_hsv(s->g_hue, s->white ? 0 : sat, bri);
        const int32_t mid = n / 2;
        for (int32_t i = 0; i < n; i++) {
            bool on;
            switch (s->pat) {
            case BPAT_HALVES:   on = s->flip ? (i < mid) : (i >= mid); break;
            case BPAT_CENTER:   on = 2 * (i > mid ? i - mid : mid - i) <= s->width; break;
            case BPAT_SEGMENTS: on = (((i * 8) / n) & 1) == (s->flip ? 1 : 0); break;
            case BPAT_CHASE:    on = (i * 4) / n == (f->music.bar_beat & 3u); break;
            default:           on = true; break;
            }
            if (on) px[i] = c;
        }
    }
    if (!(s->env > 0 || s->on_cnt)) s->white = false;

    /* snare accents at both ends */
    if (s->accent > 256) {
        led_rgb_t c = led_hsv(pal[1], sat / 3, led_gamma8((uint8_t)(s->accent >> 7)));
        for (int32_t i = 0; i < 3 && i < n; i++) {
            px[i] = led_add(px[i], c);
            px[n - 1 - i] = led_add(px[n - 1 - i], c);
        }
        s->accent = (s->accent * s->dec_accent) >> 15;
    }
    /* 808 / sub-bass glow from the centre */
    if (pr->sub_swell) {
        int32_t sub = f->music.sub;
        int32_t v = ((sub >> 7) * pr->sub_swell) >> 9;     /* up to ~half brightness */
        if (v > 0) draw_tri(px, n, (n - 1) * 128, ((n * sub) >> 15) * 128 + 256, pal[0], sat, (uint8_t)(v > 255 ? 255 : v));
    }
}

/* ---- lead ------------------------------------------------------------------ */

static int32_t pitch_pos_q8(const rshow_state_t *s, int32_t pitch, int32_t n)
{
    int32_t x = ((pitch - s->pmin) * 256) / (s->pmax - s->pmin);     /* 0..256 */
    if (x < 0) x = 0;
    if (x > 256) x = 256;
    return (n - 1) * x;
}

static void lead_draw(rshow_state_t *s, const audio_frame_t *f, const uint16_t *pal, uint8_t sat)
{
    const int32_t n = f->num_leds;
    const melody_t *mel = &f->melody;
    led_rgb_t *px = s->layer;
    led_fill(px, (uint16_t)n, LED_BLACK);

    if (mel->active) {
        int32_t p = mel->pitch_q8;
        if (p < s->pmin) s->pmin = p; else s->pmin += (p - s->pmin) >> 11;
        if (p > s->pmax) s->pmax = p; else s->pmax -= (s->pmax - p) >> 11;
        if (s->pmax - s->pmin < 12 * 256) s->pmax = s->pmin + 12 * 256;
    }
    int32_t lvl = Q15F(0.35) + ((Q15F(0.65) * (int32_t)mel->level) >> 15);
    if (mel->note_on) {
        for (int k = 0; k < s->max_sparks; k++) s->sp[k].held = false;
        spark_t *sp = &s->sp[s->sp_next];
        s->sp_next = (uint8_t)((s->sp_next + 1u) % s->max_sparks);
        sp->pos_q8 = sp->target_q8 = pitch_pos_q8(s, mel->pitch_q8, n);
        sp->amp = 32767;
        uint32_t pc = (uint32_t)((mel->pitch_q8 + 128) >> 8) % 12u;
        sp->hue = (uint16_t)((pal[2] + pc * 30u) % 360u);
        sp->held = true;
    }
    for (int k = 0; k < s->max_sparks; k++) {
        spark_t *sp = &s->sp[k];
        if (sp->held && !mel->active) sp->held = false;
        if (sp->held) {
            sp->target_q8 = pitch_pos_q8(s, mel->pitch_q8, n);
            sp->amp += (lvl - sp->amp) / 8;
        } else {
            sp->amp = (sp->amp * s->trail) >> 15;
        }
        sp->pos_q8 += (sp->target_q8 - sp->pos_q8) / 4;
        if (sp->amp < 128) continue;
        int32_t half = 300 + ((sp->amp * 330) >> 15);           /* 1.2 .. 2.5 LEDs */
        draw_tri(px, n, sp->pos_q8, half, sp->hue, sat, (uint8_t)(sp->amp >> 7));
    }
    if (mel->active) {
        uint8_t g = (uint8_t)((mel->salience >> 7) / 8);        /* <= 12 % */
        led_rgb_t c = led_hsv(pal[2], sat, led_gamma8(g));
        for (int32_t i = 0; i < n; i++) px[i] = led_add(px[i], c);
    }
}

/* ---- percussion ------------------------------------------------------------ */

static void add_pulse(rshow_state_t *s, int32_t center_q8, int32_t half_q8, int32_t amp, int32_t decay,
                      uint16_t hue, uint8_t sat)
{
    pulse_t *p = &s->pl[s->pl_next];
    s->pl_next = (uint8_t)((s->pl_next + 1u) % MAX_PULSES);
    p->center_q8 = center_q8;
    p->half_q8 = half_q8;
    p->amp = amp;
    p->decay = decay;
    p->hue = hue;
    p->sat = sat;
}

static void perc_draw(rshow_state_t *s, const audio_frame_t *f, const music_profile_t *pr, const uint16_t *pal,
                      uint8_t sat)
{
    const int32_t n = f->num_leds;
    const music_t *m = &f->music;
    led_rgb_t *px = s->layer;
    led_fill(px, (uint16_t)n, LED_BLACK);
    uint32_t sparkle = (pr->sparkle * s->sparkle) >> 8;        /* Q8 */

    if ((m->hits & (1u << HIT_KICK)) && m->w_groove < Q15F(0.5)) {
        int32_t a = 16384 + (m->hit_strength[HIT_KICK] >> 1);
        add_pulse(s, 0, 4 * 256, a, s->dec_kick, pal[0], sat);
        add_pulse(s, (n - 1) * 256, 4 * 256, a, s->dec_kick, pal[0], sat);
    }
    if (m->hits & (1u << HIT_TOM)) {
        int32_t str = m->hit_strength[HIT_TOM];
        add_pulse(s, ((n - 1) * (int32_t)m->tom_pos) >> 7, (2 * 256) + ((4 * 256 * str) >> 15),
                  16384 + (str >> 1), s->dec_tom, pal[3], sat);
    }
    if (m->hits & (1u << HIT_SNARE)) {
        int32_t str = m->hit_strength[HIT_SNARE];
        add_pulse(s, (n - 1) * 128, (3 * 256) + ((2 * 256 * str) >> 15), 16384 + (str >> 1), s->dec_snare,
                  pal[1], (uint8_t)(sat / 4));
    }
    if ((m->hits & (1u << HIT_HAT)) && sparkle) {
        uint32_t cnt = ((1u + ((2u * m->hit_strength[HIT_HAT]) >> 15)) * sparkle) >> 8;
        if (cnt == 0) cnt = 1;
        if (cnt > 6) cnt = 6;
        uint8_t v = (uint8_t)(160u + ((95u * m->hit_strength[HIT_HAT]) >> 15));
        for (uint32_t k = 0; k < cnt; k++) {
            uint16_t i;
            if (m->hat_roll) {
                s->run_pos = (uint16_t)((s->run_pos + 2u) % (uint32_t)n);   /* rolls run along the strip */
                i = s->run_pos;
            } else {
                i = (uint16_t)(dsp_rand16(&s->rnd) % (uint32_t)n);
            }
            if (s->tw[i] < v) s->tw[i] = v;
        }
    }
    if ((m->hits & (1u << HIT_CRASH)) && sparkle) s->crash = 32767;

    for (int k = 0; k < MAX_PULSES; k++) {
        pulse_t *p = &s->pl[k];
        if (p->amp < 128) {
            p->amp = 0;
            continue;
        }
        draw_tri(px, n, p->center_q8, p->half_q8, p->hue, p->sat, (uint8_t)(p->amp >> 7));
        p->amp = (p->amp * p->decay) >> 15;
    }
    for (int32_t i = 0; i < n; i++) {
        if (s->tw[i]) {
            px[i] = led_add(px[i], led_hsv(pal[3], sat / 2, led_gamma8(s->tw[i])));
            s->tw[i] = (uint8_t)(s->tw[i] - (s->tw[i] >> 4) - 1u);
        }
        if (s->crash > 256) {
            uint8_t r = (uint8_t)dsp_rand16(&s->rnd);
            uint8_t v = (uint8_t)(((s->crash >> 7) * r) >> 8);
            px[i] = led_add(px[i], led_hsv(pal[3], sat / 3, led_gamma8(v)));
        }
    }
    s->crash = (s->crash * s->dec_crash) >> 15;
}

/* ---- ambient / build ------------------------------------------------------- */

static void ambient_draw(rshow_state_t *s, const audio_frame_t *f, const uint16_t *pal, uint8_t sat)
{
    const int32_t n = f->num_leds;
    led_rgb_t *px = s->layer;
    s->phase = (uint16_t)(s->phase + 12u + (f->centroid >> 10));
    uint32_t base = ((uint32_t)(f->level >> 7) * s->ambient) >> 8;
    for (int32_t i = 0; i < n; i++) {
        uint16_t x = (uint16_t)((uint32_t)i * 65535u / (uint32_t)n + s->phase);
        uint32_t tri = x < 32768u ? x : 65535u - x;                    /* 0..32767 */
        uint8_t v = (uint8_t)((base * (96u + (tri >> 8))) >> 8);
        px[i] = led_hsv((uint16_t)((pal[0] + ((tri * 60u) >> 15)) % 360u), sat, led_gamma8(v));
    }
}

static void build_draw(const audio_frame_t *f, led_rgb_t *out, const uint16_t *pal, uint8_t sat)
{
    const int32_t n = f->num_leds;
    int32_t b = f->music.build;
    if (b < Q15F(0.4)) return;
    int32_t len = (n / 2 * (b - Q15F(0.4))) / Q15F(0.6);
    uint8_t v = (uint8_t)(70 + (f->music.hits ? 120 : 0));
    led_rgb_t c = led_hsv(pal[1], sat, led_gamma8(v));
    for (int32_t i = 0; i < len && i < n; i++) {
        out[i] = led_add(out[i], c);
        out[n - 1 - i] = led_add(out[n - 1 - i], c);
    }
}

/* ---- main ------------------------------------------------------------------ */

static void process(void *st, audio_frame_t *f)
{
    rshow_state_t *s = st;
    const music_t *m = &f->music;
    const music_profile_t *pr = music_profile(m->genre);
    const int32_t n = f->num_leds;
    uint16_t pal[4];
    uint8_t sat;
    if (s->custom) {
        pal[0] = s->hue;
        pal[1] = (uint16_t)((s->hue + 40u) % 360u);
        pal[2] = (uint16_t)((s->hue + 180u) % 360u);
        pal[3] = (uint16_t)((s->hue + 90u) % 360u);
        sat = (uint8_t)(s->sat_q8 > 255 ? 255 : s->sat_q8);
    } else {
        memcpy(pal, pr->hue, sizeof(pal));
        sat = (uint8_t)((pr->sat * s->sat_q8) >> 8);
    }

    /* groove triggers: beats, and in "snare = full" profiles the snare as well */
    if (f->beat) groove_trigger(s, f, pr, pal, f->beat_strength, false);
    if (m->hits & (1u << HIT_SNARE)) {
        if (pr->snare == 2 && m->w_groove > Q15F(0.3)) groove_trigger(s, f, pr, pal, m->hit_strength[HIT_SNARE], true);
        else if (pr->snare == 1) s->accent = 32767;
    }
    s->was_drop = m->drop;

    led_fill(f->leds, (uint16_t)n, LED_BLACK);
    groove_draw(s, f, pr, pal, sat);
    composite(f->leds, s->layer, n, m->w_groove);
    lead_draw(s, f, pal, sat);
    composite(f->leds, s->layer, n, m->w_lead);
    perc_draw(s, f, pr, pal, sat);
    int32_t wp = clamp15((int32_t)m->w_perc + ((int32_t)m->w_groove + m->w_lead + m->w_ambient) / 4);
    composite(f->leds, s->layer, n, (wp * (int32_t)pr->perc_gain) >> 8);
    ambient_draw(s, f, pal, sat);
    composite(f->leds, s->layer, n, m->w_ambient);
    if (m->scene != SCENE_SILENCE) build_draw(f, f->leds, pal, sat);
}

const stage_def_t g_stage_render_show = {
    .id = STAGE_RENDER_SHOW,
    .name = "Render: music show",
    .desc = "Scene-aware show: beat (all / some LEDs), lead note sparks, drum hits, ambient",
    .params = s_params,
    .num_params = RSHOW_P_COUNT,
    .state = &s_st,
    .reset = reset,
    .configure = configure,
    .process = process,
};
