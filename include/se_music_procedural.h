// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  procedural music source
//  Part of the semver'd public surface (see se_version.h).
// =====================================================================
//
// A seed-driven procedural music generator. The generator's *structure*
// is fixed -- a six-voice synth (saw bass, square arp, 3-saw pad, sine
// kick, noise snare, noise hi-hat) on a 4/4, 16th-note grid, in 16-bar
// sections of eight two-bar chords. Its *content and tone* are data,
// supplied as a se_music_config_t: the tempo range, the tonic pool, the
// chord / arp / drum / bass pattern banks, the per-layer gains, and each
// voice's envelope + filter. So the same generator drives entirely
// different music (different key, rhythm, harmony, balance, timbre)
// without code changes -- only the synth topology is shared.
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

// An ADSR envelope shape. Attack / decay / release in seconds; sustain is
// a 0..1 level.
typedef struct {
    float attack, decay, sustain, release;
} se_music_env_t;

// A biquad filter spec (cutoff/centre in Hz, resonance Q).
typedef struct {
    float hz, q;
} se_music_filter_t;

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

    // Per-layer master gains (relative to int16 full-scale, before the
    // mixer's music-gain pass). Keep the sum comfortably under 1.0.
    float bass_amp, arp_amp, pad_amp, kick_amp, snare_amp, hat_amp;

    // Per-voice envelope shapes.
    se_music_env_t bass_env, arp_env, pad_env, kick_env, snare_env, hat_env;

    // Per-voice filters: bass/arp/pad lowpass, snare bandpass, hat highpass.
    se_music_filter_t bass_lpf, arp_lpf, pad_lpf, snare_bpf, hat_hpf;

    // Pad colour: fractional detune of the pad's outer two saws (e.g.
    // 0.005 = +/-0.5%), and the rate (Hz) of the slow LFO that tilts the
    // pad's amplitude.
    float pad_detune;
    float pad_lfo_hz;
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
