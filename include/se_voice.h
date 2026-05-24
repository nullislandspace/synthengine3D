// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  synth voices
//  Part of the semver'd public surface (see se_version.h).
// =====================================================================
//
// A *voice* is one playing note: you trigger it (note_on), optionally
// release it (note_off), and pull audio out of it (render). This is the
// pluggable unit of synthesis. Two things drive voices:
//
//   - the procedural music generator (se_music_procedural.h) assigns one
//     voice to each of its roles (bass / arp / pad / kick / snare / hat)
//     and triggers them from its pattern sequencer; and
//
//   - a future MIDI player will keep a pool of voices and route note-on /
//     note-off events to them (with voice stealing via `active`).
//
// Both see only the `se_voice_t` vtable below, so a game can supply its
// own synthesis by implementing that struct, or use the built-in
// `se_voice_synth_t` (a configurable subtractive/noise voice) that covers
// the usual oscillator + filter + envelope palette.
//
// Polyphony / chords are expressed as *multiple voices*, never one voice
// playing several pitches — that keeps the model identical for the
// sequencer and for MIDI.
//
// All callbacks run on the mixer task: no blocking, no allocation, no
// logging. They operate at AUDIO_SAMPLE_RATE_HZ (see se_audio_source.h).

#pragma once

#include "se_audio_dsp.h"   // audio_env_t / audio_biquad_t (built-in voice state)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------
// Voice interface (the pluggable contract)
// ---------------------------------------------------------------

typedef struct se_voice_s se_voice_t;

struct se_voice_s {
    // Start (or retrigger) a note: oscillator pitch in Hz, velocity in
    // [0, 1] (scales output level). Restarts the amplitude envelope.
    // Percussion voices may ignore `freq_hz`.
    void (*note_on)(se_voice_t* v, float freq_hz, float velocity);

    // Release the note (envelope enters its release stage). May be NULL
    // for one-shot percussion that ends on its own.
    void (*note_off)(se_voice_t* v);

    // ADD this voice's output (already scaled by its own gain and the
    // note velocity) to `mix[0..frames)`, advancing all internal state.
    // The hot path: one call per block, the voice loops internally.
    void (*render)(se_voice_t* v, float* mix, size_t frames);

    // True while the voice is still producing sound (envelope not idle).
    // A sequencer / MIDI allocator uses this to skip silent voices and to
    // pick a voice to steal. May be NULL (treated as always active).
    bool (*active)(se_voice_t const* v);

    // Backend-specific state follows in the embedding struct.
};

// ---------------------------------------------------------------
// Built-in subtractive / noise voice
// ---------------------------------------------------------------

// Oscillator source for the built-in voice.
typedef enum {
    SE_OSC_SINE = 0,
    SE_OSC_SAW,
    SE_OSC_SQUARE,
    SE_OSC_TRIANGLE,
    SE_OSC_NOISE,        // white noise (ignores pitch); pair with a filter
} se_osc_kind_t;

// Post-oscillator filter for the built-in voice.
typedef enum {
    SE_FILTER_NONE = 0,
    SE_FILTER_LPF,
    SE_FILTER_HPF,
    SE_FILTER_BPF,
} se_filter_kind_t;

// An ADSR amplitude envelope: attack / decay / release in seconds,
// sustain a level in [0, 1].
typedef struct {
    float attack, decay, sustain, release;
} se_env_t;

// Maximum unison oscillators per built-in voice (a detuned stack —
// "supersaw"). 1 = a single oscillator.
#define SE_VOICE_MAX_OSC 3

// Describes a built-in voice's synthesis: oscillator(s) -> filter ->
// amplitude envelope -> gain, with two optional modulations. Passed by
// value to se_voice_synth_init (copied).
typedef struct {
    se_osc_kind_t    osc;            // waveform / noise source
    uint8_t          osc_count;      // 1..SE_VOICE_MAX_OSC; >1 = detuned unison
    float            detune;         // fractional unison spread (e.g. 0.006); ignored if osc_count==1
    se_env_t         env;            // ADSR amplitude envelope
    se_filter_kind_t filter;         // filter applied after the oscillator(s)
    float            cutoff_hz;      // filter cutoff / centre (ignored if filter==NONE)
    float            q;              // filter resonance (~0.707 = flat)
    float            gain;           // this voice's output level
    // Optional modulation (0 disables):
    float            pitch_env_amt;  // Hz added to the note, scaled by the live envelope (e.g. a kick's pitch drop)
    float            amp_lfo_hz;     // amplitude-LFO rate; amp *= (1-depth) + depth*sin(lfo)
    float            amp_lfo_depth;  // LFO depth in [0, 1]
} se_voice_spec_t;

// A built-in voice instance. Embed it (no heap) and initialise with
// se_voice_synth_init; then drive it through `base` (the se_voice_t
// vtable). One instance plays one note at a time.
typedef struct {
    se_voice_t      base;            // vtable — MUST be first (cast target)
    se_voice_spec_t spec;            // copied at init
    uint32_t        phase[SE_VOICE_MAX_OSC];
    uint32_t        noise_state;
    audio_env_t     env;
    audio_biquad_t  filter;
    bool            has_filter;
    uint32_t        lfo_phase;
    uint32_t        lfo_inc;         // phase increment for amp_lfo_hz
    float           base_freq;       // current note, set by note_on
    float           velocity;        // current note velocity, set by note_on
} se_voice_synth_t;

// Initialise a built-in voice in place from `spec` (copied). Wires up the
// se_voice_t vtable and configures the envelope + filter; no allocation.
// Re-init is a full reset (silences the voice). Safe to call any time the
// voice isn't mid-render.
void se_voice_synth_init(se_voice_synth_t* v, se_voice_spec_t const* spec);
