/**
 * @file music_profiles.c
 * @brief Genre profile table of the music show.
 */
#include "music_profiles.h"

#define Q15(x)  ((int16_t)((x) * 32767.0f))
#define Q8(x)   ((uint16_t)((x) * 256.0f))

static const music_profile_t s_profiles[GENRE_COUNT] = {
    /* AUTO is resolved by stage_music; this row is never used directly */
    [GENRE_AUTO] = {0},
    [GENRE_EDM] = {
        .name = "edm", .hue = {200, 280, 170, 320}, .sat = 255, .full_bias = Q15(0.15f),
        .patterns = BPAT_BIT(BPAT_HALVES) | BPAT_BIT(BPAT_SEGMENTS) | BPAT_BIT(BPAT_CHASE),
        .decay_style = 0, .on_ms = 90, .decay_ms = 200,
        .lead_gain = Q8(0.8f), .perc_gain = Q8(1.0f), .fill_gain = Q8(1.0f), .clarity_off = 0,
        .drop_strobe = 1, .snare = 0, .sparkle = Q8(1.5f), .sub_swell = Q8(0.3f),
    },
    [GENRE_POP] = {
        .name = "pop", .hue = {330, 40, 190, 280}, .sat = 255, .full_bias = 0,
        .patterns = BPAT_BIT(BPAT_HALVES) | BPAT_BIT(BPAT_CENTER),
        .decay_style = 0, .on_ms = 110, .decay_ms = 220,
        .lead_gain = Q8(1.1f), .perc_gain = Q8(0.9f), .fill_gain = Q8(1.0f), .clarity_off = 0,
        .drop_strobe = 0, .snare = 1, .sparkle = Q8(1.0f), .sub_swell = Q8(0.2f),
    },
    [GENRE_ROCK] = {
        .name = "rock", .hue = {20, 45, 200, 0}, .sat = 255, .full_bias = Q15(0.05f),
        .patterns = BPAT_BIT(BPAT_CENTER) | BPAT_BIT(BPAT_HALVES),
        .decay_style = 1, .on_ms = 120, .decay_ms = 260,
        .lead_gain = Q8(1.2f), .perc_gain = Q8(1.2f), .fill_gain = Q8(1.2f), .clarity_off = 0,
        .drop_strobe = 0, .snare = 1, .sparkle = Q8(1.0f), .sub_swell = Q8(0.1f),
    },
    [GENRE_METAL] = {
        .name = "metal", .hue = {0, 15, 30, 0}, .sat = 255, .full_bias = Q15(0.1f),
        .patterns = BPAT_BIT(BPAT_CENTER) | BPAT_BIT(BPAT_SEGMENTS) | BPAT_BIT(BPAT_CHASE),
        .decay_style = 0, .on_ms = 60, .decay_ms = 120,
        .lead_gain = Q8(1.3f), .perc_gain = Q8(1.3f), .fill_gain = Q8(0.7f), .clarity_off = Q15(-0.05f),
        .drop_strobe = 1, .snare = 0, .sparkle = Q8(2.0f), .sub_swell = 0,
    },
    [GENRE_RNB] = {
        .name = "rnb", .hue = {290, 320, 260, 200}, .sat = 230, .full_bias = Q15(-0.25f),
        .patterns = BPAT_BIT(BPAT_CENTER),
        .decay_style = 1, .on_ms = 160, .decay_ms = 450,
        .lead_gain = Q8(1.3f), .perc_gain = Q8(0.7f), .fill_gain = Q8(1.0f), .clarity_off = 0,
        .drop_strobe = 0, .snare = 0, .sparkle = Q8(0.5f), .sub_swell = Q8(0.5f),
    },
    [GENRE_HIPHOP] = {
        .name = "hiphop", .hue = {40, 25, 50, 280}, .sat = 255, .full_bias = Q15(-0.1f),
        .patterns = BPAT_BIT(BPAT_CENTER) | BPAT_BIT(BPAT_HALVES),
        .decay_style = 1, .on_ms = 120, .decay_ms = 220,
        .lead_gain = Q8(1.0f), .perc_gain = Q8(1.0f), .fill_gain = Q8(1.0f), .clarity_off = 0,
        .drop_strobe = 0, .snare = 1, .sparkle = Q8(1.0f), .sub_swell = Q8(0.5f),
    },
    [GENRE_TRAP] = {
        .name = "trap", .hue = {350, 275, 300, 0}, .sat = 255, .full_bias = Q15(-0.15f),
        .patterns = BPAT_BIT(BPAT_CENTER) | BPAT_BIT(BPAT_SEGMENTS),
        .decay_style = 1, .on_ms = 120, .decay_ms = 300,
        .lead_gain = Q8(0.9f), .perc_gain = Q8(1.1f), .fill_gain = Q8(0.9f), .clarity_off = 0,
        .drop_strobe = 0, .snare = 2, .sparkle = Q8(1.5f), .sub_swell = Q8(1.0f),
    },
    [GENRE_ORCHESTRAL] = {
        .name = "orchestral", .hue = {35, 20, 45, 200}, .sat = 180, .full_bias = Q15(-0.4f),
        .patterns = BPAT_BIT(BPAT_CENTER),
        .decay_style = 1, .on_ms = 200, .decay_ms = 600,
        .lead_gain = Q8(1.5f), .perc_gain = Q8(0.6f), .fill_gain = Q8(0.8f), .clarity_off = Q15(0.15f),
        .drop_strobe = 0, .snare = 0, .sparkle = Q8(0.5f), .sub_swell = 0,
    },
};

const music_profile_t *music_profile(uint8_t genre)
{
    if (genre == GENRE_AUTO || genre >= GENRE_COUNT) genre = GENRE_POP;
    return &s_profiles[genre];
}
