// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  procedural music source
//  Part of the semver'd public surface (see se_version.h).
// =====================================================================
//
// A seed-driven procedural music generator. The generator's *arrangement
// structure* is fixed -- six roles (bass, arp, pad, kick, snare, hi-hat)
// on a 4/4, 16th-note grid, in 16-bar sections of eight two-bar chords.
// Everything else is data, supplied as a se_music_config_t: the tempo
// range, the tonic pool, the chord / arp / drum / bass pattern banks, and
// a pluggable voice per role. Each role's voice is a se_voice_spec_t (the
// built-in subtractive/noise synth -- osc + filter + envelope + gain +
// mods, see se_voice.h), or a game's own se_voice_t for full custom
// synthesis. So the same generator drives entirely different music --
// different key, rhythm, harmony, balance, AND timbre -- without code
// changes; only the arrangement is shared. The same voices feed a future
// MIDI player.
//
// Same seed + same config => identical music. The generator uses a PRNG
// separate from any world generator, so toggling music never perturbs
// other seed-driven content.
//
// Returns a heap-allocated music_source_t (se_audio_source.h) ready for
// audio_mixer_set_music(); the mixer takes ownership and the source frees
// itself in its shutdown() callback.

#pragma once

#include "se_audio_source.h"
#include "se_voice.h"          // se_voice_spec_t / se_voice_t (per-role voices)
#include <stdbool.h>
#include <stdint.h>

// Fixed rhythmic grid (a config fills these structures; it does not
// resize them). 4/4 time, 16 sixteenths per bar, eight chords per
// 16-bar section.
#define SE_MUSIC_TICKS_PER_BAR       16
#define SE_MUSIC_CHORDS_PER_SECTION   8

// One chord: a triad `root_offset` semitones above the run's tonic, minor
// (is_major = 0 -> third is +3) or major (is_major = 1 -> third is +4);
// the fifth is always +7.
typedef struct {
    int8_t  root_offset;
    uint8_t is_major;
} se_music_chord_t;

// A chord progression: exactly SE_MUSIC_CHORDS_PER_SECTION chords, each
// held for two bars (one progression == one 16-bar section).
typedef struct {
    se_music_chord_t slots[SE_MUSIC_CHORDS_PER_SECTION];
} se_music_progression_t;

// One bar of arp steps. Each step indexes the current chord's triad
// lifted into octaves: 0 root, 1 third, 2 fifth, 3 root+8ve, 4 third+8ve,
// 5 fifth+8ve; -1 is a rest (no note this 16th).
typedef struct {
    int8_t step[SE_MUSIC_TICKS_PER_BAR];
} se_music_arp_pattern_t;

// A drum pattern: one 16-bit mask per voice, LSB = the bar's first 16th.
typedef struct {
    uint16_t kick;
    uint16_t snare;
    uint16_t hat;
} se_music_drum_pattern_t;

// (The ADSR envelope shape and oscillator/filter selection that used to
// live here are now part of se_voice_spec_t in se_voice.h — each role
// below carries a full voice spec rather than separate env/filter/gain
// fields.)

// The musical content + tone the generator plays. Pattern banks are
// retained BY REFERENCE -- they (and the config itself) must outlive the
// created source; point them at static storage. Every bank needs >= 1
// entry and a non-NULL pointer.
typedef struct {
    // Tempo: an integer BPM is picked per run in [bpm_min, bpm_min + bpm_span).
    uint16_t bpm_min;
    uint16_t bpm_span;

    // Tonic pool: MIDI note numbers; one is chosen per run as the key.
    int8_t const* tonics;
    int           tonic_count;

    // Pattern banks (a fresh selection is made per section / per run).
    se_music_progression_t  const* progressions;   int progression_count;
    se_music_arp_pattern_t   const* arp_patterns;   int arp_pattern_count;
    se_music_drum_pattern_t  const* drum_patterns;  int drum_pattern_count;
    uint16_t                 const* bass_patterns;  int bass_pattern_count; // 16-bit 16th masks

    // Per-role voice synthesis. Each spec is the built-in subtractive /
    // noise voice (osc + filter + ADSR + gain + optional pitch-env / amp-
    // LFO). The per-role gain that used to be a separate *_amp field now
    // lives in each spec's .gain — keep their sum comfortably under 1.0,
    // before the mixer's music-gain pass.
    se_voice_spec_t bass, arp, pad, kick, snare, hat;

    // Optional per-role custom voice. When non-NULL the generator drives
    // this voice for the role instead of building one from the spec above;
    // it is caller-owned and must outlive the source. The pad is
    // polyphonic (a three-note chord = three voices), so it is spec-only —
    // pad_voice is ignored. NULL = use the spec.
    se_voice_t* bass_voice;
    se_voice_t* arp_voice;
    se_voice_t* kick_voice;
    se_voice_t* snare_voice;
    se_voice_t* hat_voice;

    // Pad voicing: fractional detune applied to the root and fifth pad
    // voices (e.g. 0.005 = +/-0.5%) for width. The pad's amplitude LFO is
    // set per the pad spec's amp_lfo_hz / amp_lfo_depth.
    float pad_detune;
} se_music_config_t;

// The built-in synthwave personality: ~110 BPM minor-key outrun. Returns
// a pointer to static, always-valid config data. Pass it (or NULL, which
// selects it) to music_procedural_create() for the engine's default sound.
se_music_config_t const* se_music_synthwave_preset(void);

// Create a procedural music source driven by `cfg` (NULL selects the
// synthwave preset). `cfg` and its pattern banks are retained by
// reference and must outlive the source. Returns NULL on allocation
// failure. Same seed + same config => identical music.
music_source_t* music_procedural_create(se_music_config_t const* cfg, uint32_t seed);
