/**
 * @file algorithms.c
 * @brief Algorithm definitions: each algorithm is just an ordered list of stages.
 *
 * To experiment, add/remove/reorder entries in these arrays. Stages communicate only
 * through audio_frame_t, so any analysis stage can feed any render stage.
 */
#include "pipeline.h"

/* 0: scene-aware show: the beat drives all / some LEDs while the beat is clear, the
 *    lead voice draws note sparks, drum fills / breaks / build-ups get their own visuals */
static const stage_def_t *const s_algo_music_show[] = {
    &g_stage_frontend,
    &g_stage_filter_a,
    &g_stage_envelope,
    &g_stage_agc,
    &g_stage_spectrum,
    &g_stage_beat,
    &g_stage_melody,
    &g_stage_music,
    &g_stage_render_show,
};

/* 1: all LEDs turn on/off on the main beat (bass onsets + tempo tracking) */
static const stage_def_t *const s_algo_beat_all[] = {
    &g_stage_frontend,
    &g_stage_filter_a,
    &g_stage_envelope,
    &g_stage_agc,
    &g_stage_beat,
    &g_stage_render_beat_flash,
};

/* 2: which LEDs = frequency band, how many = amplitude in that band */
static const stage_def_t *const s_algo_spectrum_bars[] = {
    &g_stage_frontend,
    &g_stage_filter_a,
    &g_stage_filter_b,
    &g_stage_envelope,
    &g_stage_agc,
    &g_stage_spectrum,
    &g_stage_render_spectrum,
};

/* 3: on every beat a segment lights up; position = spectral centroid,
 *    width = beat strength */
static const stage_def_t *const s_algo_beat_segments[] = {
    &g_stage_frontend,
    &g_stage_filter_a,
    &g_stage_envelope,
    &g_stage_agc,
    &g_stage_spectrum,
    &g_stage_beat,
    &g_stage_render_beat_segments,
};

#define ALGO(id_, name_, desc_, arr_) \
    [id_] = {(id_), (name_), (desc_), (arr_), (uint8_t)(sizeof(arr_) / sizeof((arr_)[0]))}

const algorithm_def_t g_algorithms[NUM_ALGORITHMS] = {
    ALGO(ALGO_MUSIC_SHOW, "Music show",
         "Beat (all / some LEDs), lead notes, drum fills, build-ups and drops; genre profiles",
         s_algo_music_show),
    ALGO(ALGO_BEAT_ALL, "Beat (all LEDs)",
         "All LEDs switch on with the main beat (tempo locked)", s_algo_beat_all),
    ALGO(ALGO_SPECTRUM_BARS, "Spectrum bars",
         "One bar per frequency band; bar length follows the band level", s_algo_spectrum_bars),
    ALGO(ALGO_BEAT_SEGMENTS, "Beat segments",
         "Beat pulses: position from spectral centroid, width from beat strength", s_algo_beat_segments),
};

_Static_assert(sizeof(g_algorithms) / sizeof(g_algorithms[0]) == NUM_ALGORITHMS, "NUM_ALGORITHMS mismatch");
