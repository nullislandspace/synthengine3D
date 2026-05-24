// =====================================================================
//  SynthEngine3D  --  built-in synth voice (see se_voice.h)
// ---------------------------------------------------------------------
//  The configurable subtractive/noise voice the procedural generator and
//  the (future) MIDI player use by default: oscillator(s) -> optional
//  filter -> ADSR -> gain, with an optional pitch envelope (kick drop)
//  and amplitude LFO (pad shimmer). Runs on the mixer task; no alloc, no
//  blocking. The note lifecycle (note_on/off) and the block renderer are
//  the se_voice_t vtable a game can also implement itself.
// =====================================================================

#include "se_voice.h"

#include "se_audio_dsp.h"

#include <string.h>

// --- vtable callbacks --------------------------------------------------

static void synth_note_on(se_voice_t* base, float freq_hz, float velocity) {
    se_voice_synth_t* v = (se_voice_synth_t*)base;
    v->base_freq = freq_hz;
    v->velocity  = velocity;
    audio_env_trigger(&v->env);
    // A pitch-enveloped voice (e.g. a kick) resets phase so every hit
    // starts from the same point — a consistent attack transient.
    // Sustained pitched voices let phase free-run across retriggers.
    if (v->spec.pitch_env_amt != 0.0f) {
        for (int i = 0; i < SE_VOICE_MAX_OSC; i++) v->phase[i] = 0u;
    }
}

static void synth_note_off(se_voice_t* base) {
    audio_env_release(&((se_voice_synth_t*)base)->env);
}

static bool synth_active(se_voice_t const* base) {
    return !audio_env_is_idle(&((se_voice_synth_t const*)base)->env);
}

// One oscillator sample from `kind` at `phase` (noise pulls from *ns).
static inline float osc_sample(se_osc_kind_t kind, uint32_t phase, uint32_t* ns) {
    switch (kind) {
        case SE_OSC_SINE:     return audio_dsp_sin(phase);
        case SE_OSC_SQUARE:   return audio_dsp_square(phase);
        case SE_OSC_TRIANGLE: return audio_dsp_triangle(phase);
        case SE_OSC_NOISE:    return audio_dsp_noise(ns);
        case SE_OSC_SAW:
        default:              return audio_dsp_saw(phase);
    }
}

static void synth_render(se_voice_t* base, float* mix, size_t frames) {
    se_voice_synth_t*      v  = (se_voice_synth_t*)base;
    se_voice_spec_t const* sp = &v->spec;

    int n_osc = (int)sp->osc_count;
    if (n_osc < 1)                 n_osc = 1;
    if (n_osc > SE_VOICE_MAX_OSC)  n_osc = SE_VOICE_MAX_OSC;
    bool  const noise   = (sp->osc == SE_OSC_NOISE);
    float const inv_osc = 1.0f / (float)n_osc;
    float const vg      = v->velocity * sp->gain;

    for (size_t i = 0; i < frames; i++) {
        float const e = audio_env_tick(&v->env);
        // Pitch envelope: add an env-scaled Hz offset to the note (a
        // kick's fast downward sweep when pitch_env_amt > 0).
        float const f = v->base_freq + sp->pitch_env_amt * e;

        float s;
        if (noise) {
            s = audio_dsp_noise(&v->noise_state);
        } else {
            s = 0.0f;
            for (int o = 0; o < n_osc; o++) {
                // Unison detune: spread the oscillators across +/- detune.
                float of = f;
                if (n_osc > 1) {
                    float const spread = sp->detune *
                        ((float)o * (2.0f / (float)(n_osc - 1)) - 1.0f);
                    of = f * (1.0f + spread);
                }
                s += osc_sample(sp->osc, v->phase[o], &v->noise_state);
                v->phase[o] += audio_dsp_phase_inc(of);
            }
            s *= inv_osc;
        }

        if (v->has_filter) s = audio_biquad_tick(&v->filter, s);

        if (sp->amp_lfo_hz > 0.0f) {
            float const lfo = audio_dsp_sin(v->lfo_phase);
            s *= (1.0f - sp->amp_lfo_depth) + sp->amp_lfo_depth * lfo;
            v->lfo_phase += v->lfo_inc;
        }

        mix[i] += s * e * vg;
    }
}

// --- init --------------------------------------------------------------

void se_voice_synth_init(se_voice_synth_t* v, se_voice_spec_t const* spec) {
    memset(v, 0, sizeof(*v));
    v->spec          = *spec;
    v->base.note_on  = synth_note_on;
    v->base.note_off = synth_note_off;
    v->base.render   = synth_render;
    v->base.active   = synth_active;

    audio_env_configure(&v->env, spec->env.attack, spec->env.decay,
                        spec->env.sustain, spec->env.release);
    audio_env_reset(&v->env);

    switch (spec->filter) {
        case SE_FILTER_LPF: audio_biquad_lpf(&v->filter, spec->cutoff_hz, spec->q); v->has_filter = true;  break;
        case SE_FILTER_HPF: audio_biquad_hpf(&v->filter, spec->cutoff_hz, spec->q); v->has_filter = true;  break;
        case SE_FILTER_BPF: audio_biquad_bpf(&v->filter, spec->cutoff_hz, spec->q); v->has_filter = true;  break;
        case SE_FILTER_NONE:
        default:            v->has_filter = false; break;
    }
    audio_biquad_reset(&v->filter);

    v->lfo_inc = (spec->amp_lfo_hz > 0.0f) ? audio_dsp_phase_inc(spec->amp_lfo_hz) : 0u;
}
