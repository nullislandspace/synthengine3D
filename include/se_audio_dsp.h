// Audio DSP primitives shared by the procedural music generator and
// the procedural SFX generators. Everything in this module runs on
// the mixer task — no allocations on the hot path, no blocking
// calls, no logging.
//
// Phase accumulators use a 32-bit fixed-point representation: the
// integer count of cycles modulo 1.0 is held in the top bits and
// fractional position in the rest, so an addition of `phase_inc`
// per sample wraps cheaply. The phase increment for frequency `f`
// at the project sample rate is `(uint32_t)(f * 4294967296 /
// AUDIO_SAMPLE_RATE_HZ)`.
//
// The sine lookup table is allocated in PSRAM once at boot via
// `audio_dsp_init()`. `audio_dsp_init()` is idempotent and may be
// called from any task; subsequent calls return ESP_OK without
// reallocating.

#pragma once

#include "se_audio_source.h"  // AUDIO_SAMPLE_RATE_HZ

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------
// Init
// ---------------------------------------------------------------

esp_err_t audio_dsp_init(void);

// ---------------------------------------------------------------
// Phase / oscillators
// ---------------------------------------------------------------

// Convert frequency (Hz) to a 32-bit phase increment.
static inline uint32_t audio_dsp_phase_inc(float freq_hz) {
    // 4294967296.0 == 2^32. Multiplying float by 2^32/SR gives a
    // 32-bit phase delta per sample.
    float const scale = 4294967296.0f / (float)AUDIO_SAMPLE_RATE_HZ;
    float       inc   = freq_hz * scale;
    if (inc < 0.0f) inc = 0.0f;
    if (inc > 4294967295.0f) inc = 4294967295.0f;
    return (uint32_t)inc;
}

// Returns a sample in [-1.0, 1.0] (approx) from a 1024-entry int16
// lookup, top 10 bits of `phase` index the table. Caller advances
// `phase` separately.
float audio_dsp_sin(uint32_t phase);

// Cheap saw in [-1.0, 1.0] from top bits of phase (just a cast).
static inline float audio_dsp_saw(uint32_t phase) {
    int32_t s = (int32_t)phase;          // signed reinterpretation
    return (float)s * (1.0f / 2147483648.0f);
}

// Square: positive or negative depending on top bit.
static inline float audio_dsp_square(uint32_t phase) {
    return (phase & 0x80000000u) ? 1.0f : -1.0f;
}

// Triangle: |saw| folded.
static inline float audio_dsp_triangle(uint32_t phase) {
    float s = audio_dsp_saw(phase);
    return (s < 0.0f ? -s : s) * 2.0f - 1.0f;
}

// White-noise generator. Caller owns the state (a single uint32_t).
// Returns a float in [-1.0, 1.0].
static inline float audio_dsp_noise(uint32_t* state) {
    uint32_t x = *state;
    if (x == 0u) x = 0x12345678u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

// ---------------------------------------------------------------
// ADSR envelope
// ---------------------------------------------------------------

typedef enum {
    AUDIO_ENV_IDLE = 0,
    AUDIO_ENV_ATTACK,
    AUDIO_ENV_DECAY,
    AUDIO_ENV_SUSTAIN,
    AUDIO_ENV_RELEASE,
} audio_env_stage_t;

typedef struct {
    audio_env_stage_t stage;
    float             level;     // current envelope value, 0..1
    float             sustain;   // sustain level, 0..1
    float             attack_per_sample;   // delta during ATTACK
    float             decay_per_sample;    // delta during DECAY
    float             release_per_sample;  // delta during RELEASE
} audio_env_t;

// Configure an envelope with times in seconds and a sustain level
// in [0, 1]. `attack`/`decay`/`release` are linear ramps. The
// envelope starts in IDLE — call `audio_env_trigger()` to begin.
void audio_env_configure(audio_env_t* e, float attack_s, float decay_s,
                         float sustain_level, float release_s);

// Trigger (re-trigger) the envelope: starts at 0, runs ATTACK→DECAY→
// SUSTAIN. The envelope holds at SUSTAIN until `audio_env_release()`
// is called.
void audio_env_trigger(audio_env_t* e);

// Begin RELEASE from the current level toward 0.
void audio_env_release(audio_env_t* e);

// Hard reset to IDLE (level = 0). Use for one-shot SFX where the
// envelope reaching 0 is the natural end of the voice.
static inline void audio_env_reset(audio_env_t* e) {
    e->stage = AUDIO_ENV_IDLE;
    e->level = 0.0f;
}

// Advance the envelope by one sample and return the new level.
float audio_env_tick(audio_env_t* e);

static inline bool audio_env_is_idle(audio_env_t const* e) {
    return e->stage == AUDIO_ENV_IDLE;
}

// ---------------------------------------------------------------
// Biquad filter (RBJ cookbook, direct form 1)
// ---------------------------------------------------------------

typedef struct {
    float a1, a2;            // feedback (a0 normalised to 1)
    float b0, b1, b2;        // feedforward
    float x1, x2, y1, y2;    // history
} audio_biquad_t;

// Configure as lowpass / highpass / bandpass with cutoff `fc` in Hz
// and resonance `q` (~0.707 = no peak, higher = more resonant).
// All run at AUDIO_SAMPLE_RATE_HZ.
//
// **Coefficient update only.** The filter's history (x1/x2/y1/y2)
// is preserved across the call so a per-chunk retune (e.g.
// engine-hum LPF cutoff sliding with ship speed) stays continuous —
// resetting history would produce a click at every retune. Callers
// that need a fresh start (e.g. a freshly stack-allocated filter
// before its first use) should also call `audio_biquad_reset`
// explicitly; heap_caps_calloc / static zero-init already give you
// zero history so an explicit reset is usually unnecessary.
void audio_biquad_lpf(audio_biquad_t* f, float fc, float q);
void audio_biquad_hpf(audio_biquad_t* f, float fc, float q);
void audio_biquad_bpf(audio_biquad_t* f, float fc, float q);

// Process one sample through the filter.
static inline float audio_biquad_tick(audio_biquad_t* f, float x) {
    float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2
              - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x;
    f->y2 = f->y1; f->y1 = y;
    return y;
}

// Reset filter history (clears transients before reusing the filter).
static inline void audio_biquad_reset(audio_biquad_t* f) {
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0f;
}

// ---------------------------------------------------------------
// Output conversion
// ---------------------------------------------------------------

// Soft-clip a float sample into the safe range and convert to int16.
// Uses a fast cubic clipper that's gentle around the limits and
// hard-clamps beyond ~1.5.
static inline int16_t audio_dsp_to_s16(float x) {
    if (x >  1.5f) x =  1.5f;
    if (x < -1.5f) x = -1.5f;
    // Cubic soft clip in [-1, 1]; gain falls off near the edges.
    float const y = x - (x * x * x) * (1.0f / 3.0f);
    int32_t     i = (int32_t)(y * 32767.0f);
    if (i >  32767)  i =  32767;
    if (i < -32768)  i = -32768;
    return (int16_t)i;
}
