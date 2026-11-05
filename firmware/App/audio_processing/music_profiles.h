/**
 * @file music_profiles.h
 * @brief Genre profiles of the music show (stage_music + stage_render_show).
 *
 * A profile does not change what is detected, only how strongly each situation is
 * weighted and how it looks: palette, "all LEDs vs some LEDs" bias, flash style,
 * percussion / lead emphasis. The music stage resolves GENRE_AUTO to a concrete
 * profile and publishes it in audio_frame_t.music.genre for the renderer.
 */
#ifndef MUSIC_PROFILES_H
#define MUSIC_PROFILES_H

#include <stdint.h>

/** Genres; the order is the "genre" parameter's enum order (persistent). */
typedef enum {
    GENRE_AUTO = 0,
    GENRE_EDM,
    GENRE_POP,
    GENRE_ROCK,
    GENRE_METAL,
    GENRE_RNB,
    GENRE_HIPHOP,
    GENRE_TRAP,
    GENRE_ORCHESTRAL,
    GENRE_COUNT
} genre_t;

#define GENRE_ENUM_OPTS     "auto|edm|pop|rock|metal|rnb|hiphop|trap|orchestral"

/** "Some LEDs" beat patterns (bit mask in music_profile_t.patterns). */
typedef enum {
    BPAT_HALVES = 0,         /**< left / right half, alternating per beat       */
    BPAT_CENTER,             /**< burst from the centre, width = beat strength  */
    BPAT_SEGMENTS,           /**< every other of 8 segments, alternating         */
    BPAT_CHASE,              /**< one segment per beat of the bar, moving        */
    BPAT_COUNT
} beat_pattern_t;

#define BPAT_BIT(p)          (1u << (p))

typedef struct {
    const char *name;
    uint16_t hue[4];        /**< palette: groove, groove accent, lead, percussion  */
    uint8_t  sat;
    int16_t  full_bias;     /**< Q15, added to the "all LEDs" score of a beat      */
    uint8_t  patterns;      /**< allowed "some LEDs" patterns (BPAT_BIT mask)       */
    uint8_t  decay_style;   /**< 0 = on/off for on_ms, 1 = exponential decay       */
    uint16_t on_ms;
    uint16_t decay_ms;
    uint16_t lead_gain;     /**< Q8 (256 = 1.0)                                    */
    uint16_t perc_gain;     /**< Q8                                                */
    uint16_t fill_gain;     /**< Q8, fill detection sensitivity                    */
    int16_t  clarity_off;   /**< Q15, added to the clarity threshold               */
    uint8_t  drop_strobe;   /**< first beat of a drop = white strobe               */
    uint8_t  snare;         /**< 0 = ignore, 1 = accents at the ends, 2 = full flash */
    uint16_t sparkle;       /**< Q8, hi-hat twinkles                               */
    uint16_t sub_swell;     /**< Q8, 808 / sub-bass glow                           */
} music_profile_t;

/** Profile of @p genre (GENRE_AUTO / out of range -> the pop profile). */
const music_profile_t *music_profile(uint8_t genre);

#endif /* MUSIC_PROFILES_H */
