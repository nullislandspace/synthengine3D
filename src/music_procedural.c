#include "se_music_procedural.h"

#include "se_audio_dsp.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static char const TAG[] = "music_proc";

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

// ---------------------------------------------------------------
// Fixed rhythmic structure (the config fills it; it does not resize it).
// The grid is 4/4, 16 sixteenths per bar; a chord holds two bars; eight
// chords make a 16-bar section.
// ---------------------------------------------------------------
#define TICKS_PER_BAR      SE_MUSIC_TICKS_PER_BAR
#define BARS_PER_CHORD     2
#define CHORDS_PER_SECTION SE_MUSIC_CHORDS_PER_SECTION
#define BARS_PER_SECTION   (CHORDS_PER_SECTION * BARS_PER_CHORD)

// ===============================================================
// Built-in "synthwave" preset content. This is the data the engine ships;
// a game can supply its own se_music_config_t to play something else.
// ===============================================================

// Chord bank — eight 16-bar progressions in natural minor. Scale-degree
// shortcuts: `root_offset` semitones above the tonic, `is_major` picks the
// third (minor +3 / major +4).
#define i_   {0, 0}
#define III_ {3, 1}
#define iv_  {5, 0}
#define v_   {7, 0}
#define V_   {7, 1}   // Picardy v -> V for tension lifts
#define VI_  {8, 1}
#define VII_ {10, 1}

static se_music_progression_t const g_synth_progressions[] = {
    { { i_, VII_, VI_, VII_, i_, VII_, VI_, VII_ } },   // i-VII-VI-VII ("Drive")
    { { i_, VI_, III_, VII_, i_, VI_, III_, VII_ } },   // i-VI-III-VII ("Outrun")
    { { i_, v_, VI_, VII_, i_, v_, VI_, VII_ } },       // i-v-VI-VII
    { { i_, iv_, VII_, III_, i_, iv_, VII_, III_ } },   // i-iv-VII-III
    { { i_, VII_, v_, VI_, i_, VII_, v_, VI_ } },       // i-VII-v-VI
    { { VI_, VII_, i_, i_, VI_, VII_, i_, i_ } },       // VI-VII-i-i (climb)
    { { i_, VI_, iv_, V_, i_, VI_, iv_, V_ } },         // i-VI-iv-V (power-pop)
    { { i_, III_, VII_, v_, i_, III_, VII_, v_ } },     // i-III-VII-v
};

// Arp pattern bank. One bar of 16 sixteenths; step indexes the chord triad
// lifted into octaves (0..5), -1 = rest.
static se_music_arp_pattern_t const g_synth_arps[] = {
    { { 0, 2, 1, 2, 3, 2, 1, 2,  0, 2, 1, 2, 3, 2, 1, 2 } },  // classic up-down
    { { 0,-1, 1,-1, 2,-1, 3,-1,  4,-1, 3,-1, 2,-1, 1,-1 } },  // sparse ascending
    { { 0, 3, 0, 3, 1, 4, 1, 4,  2, 5, 2, 5, 1, 4, 1, 4 } },  // octave bounce
    { { 0,-1,-1, 2, 1,-1, 3,-1,  0,-1, 2,-1,-1, 3, 1,-1 } },  // syncopated/sparse
    { { 0, 1, 2, 3, 4, 3, 2, 1,  0, 1, 2, 3, 4, 5, 4, 3 } },  // 1-3-5-8 climb/fall
    { { 0, 0, 2, 0, 1, 0, 3, 0,  0, 0, 2, 0, 1, 0, 4, 0 } },  // pulsing root + accents
};

// Drum pattern bank — bit-per-16th masks (LSB = 16th 0).
static se_music_drum_pattern_t const g_synth_drums[] = {
    { .kick = 0x0101u, .snare = 0x1010u, .hat = 0x5555u },  // four-on-the-floor
    { .kick = 0x5555u, .snare = 0x1010u, .hat = 0xAAAAu },  // pumping eighths
    { .kick = 0x0001u, .snare = 0x0100u, .hat = 0x5555u },  // half-time
    { .kick = 0x1111u, .snare = 0x1010u, .hat = 0xFFFFu },  // driving 16th hats
    { .kick = 0x0411u, .snare = 0x1010u, .hat = 0x5555u },  // broken/syncopated
    { .kick = 0x0101u, .snare = 0x0100u, .hat = 0xAAAAu },  // sparse/airy
};

// Bass rhythm bank — 16-bit 16th masks; on each set bit the bass pulses
// the chord root an octave down.
static uint16_t const g_synth_bass[] = {
    0x5555u,   // steady eighth-note pulse
    0xFFFFu,   // driving sixteenths ("outrun chase")
    0x1111u,   // laid-back quarter notes
    0x9999u,   // galloping
};

// Tonic pool — synthwave-friendly mid-range (A2..G3).
static int8_t const g_synth_tonics[] = { 45, 47, 48, 50, 52, 53, 55 };

static se_music_config_t const g_synthwave_preset = {
    .bpm_min = 100u, .bpm_span = 19u,              // 100..118 BPM
    .tonics = g_synth_tonics, .tonic_count = ARRAY_LEN(g_synth_tonics),
    .progressions  = g_synth_progressions, .progression_count = ARRAY_LEN(g_synth_progressions),
    .arp_patterns  = g_synth_arps,         .arp_pattern_count  = ARRAY_LEN(g_synth_arps),
    .drum_patterns = g_synth_drums,        .drum_pattern_count = ARRAY_LEN(g_synth_drums),
    .bass_patterns = g_synth_bass,         .bass_pattern_count = ARRAY_LEN(g_synth_bass),
    // Layer gains (sum well under 1.0 before the mixer's ~30% music pass).
    .bass_amp = 0.30f, .arp_amp = 0.16f, .pad_amp = 0.14f,
    .kick_amp = 0.45f, .snare_amp = 0.32f, .hat_amp = 0.10f,
    // Voice envelopes (attack, decay, sustain, release).
    .bass_env  = { 0.005f, 0.18f, 0.5f, 0.06f },
    .arp_env   = { 0.002f, 0.08f, 0.0f, 0.04f },
    .pad_env   = { 0.40f,  0.30f, 0.7f, 1.20f },
    .kick_env  = { 0.001f, 0.08f, 0.0f, 0.001f },
    .snare_env = { 0.001f, 0.08f, 0.0f, 0.001f },
    .hat_env   = { 0.001f, 0.03f, 0.0f, 0.001f },
    // Voice filters: bass dark, arp bright, pad warm; snare bandpass, hat highpass.
    .bass_lpf  = { 600.0f,  0.9f },
    .arp_lpf   = { 2400.0f, 0.7f },
    .pad_lpf   = { 1200.0f, 0.7f },
    .snare_bpf = { 1800.0f, 0.9f },
    .hat_hpf   = { 7000.0f, 0.8f },
    .pad_detune = 0.005f, .pad_lfo_hz = 0.4f,
};

se_music_config_t const* se_music_synthwave_preset(void) {
    return &g_synthwave_preset;
}

// ---------------------------------------------------------------
// PRNG (xorshift32) and a couple of helpers
// ---------------------------------------------------------------

static uint32_t prng_next(uint32_t* s) {
    uint32_t x = *s;
    if (x == 0u) x = 0x12345678u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}
static uint32_t prng_range(uint32_t* s, uint32_t n) {
    return n == 0 ? 0 : (prng_next(s) % n);
}

static float midi_to_hz(int midi) {
    return 440.0f * powf(2.0f, ((float)midi - 69.0f) / 12.0f);
}

// Get the three chord-tone MIDI notes for a chord, relative to
// `root_midi`. tone[0]=root, tone[1]=third, tone[2]=fifth.
static void chord_tones(se_music_chord_t c, int root_midi, int tones_out[3]) {
    int const root = root_midi + c.root_offset;
    int const third = root + (c.is_major ? 4 : 3);
    int const fifth = root + 7;
    tones_out[0] = root;
    tones_out[1] = third;
    tones_out[2] = fifth;
}

// ---------------------------------------------------------------
// Voice state
// ---------------------------------------------------------------

typedef struct {
    music_source_t base;

    se_music_config_t const* cfg;   // content + tone (retained by reference)

    uint32_t prng;

    // Tonal centre for the run.
    int     root_midi;

    // Timing.
    double  samples_per_16th;  // per-run, derived from the chosen BPM
    double  sample_pos;        // double for sub-sample drift accuracy
    double  next_tick_at;
    uint16_t tick_in_bar;      // 0..15
    uint16_t bar_in_section;   // 0..15
    uint32_t section_index;

    // Current section's progression + selected pattern set.
    se_music_progression_t  prog;
    se_music_drum_pattern_t drums;
    se_music_arp_pattern_t  arp;            // this section's arp pattern
    uint8_t        arp_index;      // its index in the arp bank (no-repeat)
    int            arp_octave;     // 0 or +12 — per-section arp transpose
    uint16_t       bass_mask;      // this section's bass rhythm
    bool           layer_bass;
    bool           layer_arp;
    bool           layer_pad;
    bool           layer_drums;

    // Bass voice.
    uint32_t       bass_phase;
    audio_env_t    bass_env;
    audio_biquad_t bass_lpf;
    float          bass_freq;

    // Arp voice.
    uint32_t       arp_phase;
    audio_env_t    arp_env;
    audio_biquad_t arp_lpf;
    float          arp_freq;

    // Pad voice — two detuned saws.
    uint32_t       pad_phase_a;
    uint32_t       pad_phase_b;
    uint32_t       pad_phase_c;   // fifth on top
    audio_env_t    pad_env;
    audio_biquad_t pad_lpf;
    float          pad_freq_a;
    float          pad_freq_b;
    float          pad_freq_c;
    uint32_t       pad_lfo_phase;
    int8_t         pad_last_chord_index;

    // Drums.
    uint32_t       kick_phase;
    audio_env_t    kick_env;
    uint32_t       snare_noise;
    audio_env_t    snare_env;
    audio_biquad_t snare_bpf;
    uint32_t       hat_noise;
    audio_env_t    hat_env;
    audio_biquad_t hat_hpf;
} music_proc_t;

// ---------------------------------------------------------------
// Section / chord / tick logic
// ---------------------------------------------------------------

static void start_new_section(music_proc_t* m) {
    se_music_config_t const* cfg = m->cfg;

    // Pick a fresh progression. Try to avoid an immediate repeat
    // (one re-roll is enough — pleasant cadence variation).
    se_music_progression_t const* candidate =
        &cfg->progressions[prng_range(&m->prng, (uint32_t)cfg->progression_count)];
    if (memcmp(candidate, &m->prog, sizeof(m->prog)) == 0) {
        candidate = &cfg->progressions[prng_range(&m->prng, (uint32_t)cfg->progression_count)];
    }
    m->prog = *candidate;

    // Drums: most sections keep the same pattern; ~25% switch.
    if ((prng_next(&m->prng) & 3u) == 0u) {
        m->drums = cfg->drum_patterns[prng_range(&m->prng, (uint32_t)cfg->drum_pattern_count)];
    }

    // Arp: a fresh pattern every section, with one re-roll to dodge
    // an immediate repeat. Roughly a third of sections also lift the
    // whole arp an octave for a brighter, sparklier passage.
    {
        uint8_t idx = (uint8_t)prng_range(&m->prng, (uint32_t)cfg->arp_pattern_count);
        if (idx == m->arp_index) {
            idx = (uint8_t)prng_range(&m->prng, (uint32_t)cfg->arp_pattern_count);
        }
        m->arp_index = idx;
        m->arp       = cfg->arp_patterns[idx];
    }
    m->arp_octave = ((prng_next(&m->prng) % 3u) == 0u) ? 12 : 0;

    // Bass: pick a rhythm for the section's low end.
    m->bass_mask = cfg->bass_patterns[prng_range(&m->prng, (uint32_t)cfg->bass_pattern_count)];

    // Layer mutation — every section may drop or restore one of
    // the harmonic layers. Drums and bass stay through the whole
    // run by default; arp/pad come and go for dynamics.
    if ((prng_next(&m->prng) & 1u) == 0u) {
        m->layer_arp = !m->layer_arp;
    }
    if ((prng_next(&m->prng) & 3u) == 0u) {
        m->layer_pad = !m->layer_pad;
    }
    // Keep at least one harmonic layer alive — if we just turned
    // both arp and pad off, force pad back on.
    if (!m->layer_arp && !m->layer_pad) m->layer_pad = true;

    m->section_index++;
    m->pad_last_chord_index = -1;  // force pad to retrigger
}

static se_music_chord_t current_chord(music_proc_t const* m) {
    int const chord_index = (m->bar_in_section / BARS_PER_CHORD) % CHORDS_PER_SECTION;
    return m->prog.slots[chord_index];
}

// Fired at the start of each 16th-note. Handles bar/section
// rollovers and (re-)triggers per-tick voice events.
static void on_tick(music_proc_t* m) {
    se_music_chord_t const c = current_chord(m);
    int tones[3];
    chord_tones(c, m->root_midi, tones);

    // Bass: pulse the chord root an octave down on every 16th the
    // section's bass pattern marks. The last 16th of a bar has a
    // chance to walk up to the fifth instead — a small lead-in to
    // the next chord.
    if (m->layer_bass) {
        uint16_t const bit = (uint16_t)(1u << m->tick_in_bar);
        if (m->bass_mask & bit) {
            bool const walk = (m->tick_in_bar == TICKS_PER_BAR - 1)
                           && ((prng_next(&m->prng) & 3u) == 0u);
            int const bass_midi = (walk ? tones[2] : tones[0]) - 12;
            m->bass_freq = midi_to_hz(bass_midi);
            audio_env_trigger(&m->bass_env);
        }
    }

    // Arp: walk the section's arp pattern. A negative step is a
    // rest (no trigger); the rest are chord tones lifted into
    // octaves, optionally transposed up an octave for the section.
    if (m->layer_arp) {
        int const step = m->arp.step[m->tick_in_bar];
        if (step >= 0) {
            int midi;
            switch (step) {
                case 0:  midi = tones[0];      break;
                case 1:  midi = tones[1];      break;
                case 2:  midi = tones[2];      break;
                case 3:  midi = tones[0] + 12; break;
                case 4:  midi = tones[1] + 12; break;
                default: midi = tones[2] + 12; break;
            }
            m->arp_freq = midi_to_hz(midi + m->arp_octave);
            audio_env_trigger(&m->arp_env);
        }
    }

    // Pad: retrigger on chord changes only.
    int8_t const chord_index = (int8_t)((m->bar_in_section / BARS_PER_CHORD) % CHORDS_PER_SECTION);
    if (m->layer_pad && chord_index != m->pad_last_chord_index) {
        // Three voices: root, third, fifth, all in the mid-octave.
        m->pad_freq_a = midi_to_hz(tones[0]);
        m->pad_freq_b = midi_to_hz(tones[1]);
        m->pad_freq_c = midi_to_hz(tones[2]);
        audio_env_trigger(&m->pad_env);
        m->pad_last_chord_index = chord_index;
    }

    // Drums.
    if (m->layer_drums) {
        uint16_t const bit = (uint16_t)(1u << m->tick_in_bar);
        if (m->drums.kick & bit) {
            audio_env_trigger(&m->kick_env);
            m->kick_phase = 0;
        }
        if (m->drums.snare & bit) {
            audio_env_trigger(&m->snare_env);
        }
        if (m->drums.hat & bit) {
            audio_env_trigger(&m->hat_env);
        }
    }

    // Advance bar / section counters.
    m->tick_in_bar++;
    if (m->tick_in_bar >= TICKS_PER_BAR) {
        m->tick_in_bar = 0;
        m->bar_in_section++;
        if (m->bar_in_section >= BARS_PER_SECTION) {
            m->bar_in_section = 0;
            start_new_section(m);
        }
    }
}

// ---------------------------------------------------------------
// Render
// ---------------------------------------------------------------

static float render_sample(music_proc_t* m) {
    se_music_config_t const* cfg = m->cfg;
    float out = 0.0f;

    // Bass — saw through lowpass.
    {
        uint32_t const inc = audio_dsp_phase_inc(m->bass_freq);
        float const e = audio_env_tick(&m->bass_env);
        float       s = audio_dsp_saw(m->bass_phase);
        s = audio_biquad_tick(&m->bass_lpf, s);
        m->bass_phase += inc;
        out += s * e * cfg->bass_amp;
    }

    // Arp — square through lowpass.
    {
        uint32_t const inc = audio_dsp_phase_inc(m->arp_freq);
        float const e = audio_env_tick(&m->arp_env);
        float       s = audio_dsp_square(m->arp_phase);
        s = audio_biquad_tick(&m->arp_lpf, s);
        m->arp_phase += inc;
        out += s * e * cfg->arp_amp;
    }

    // Pad — three slightly-detuned saws (root + third + fifth)
    // through a lowpass whose amplitude tilts on a slow LFO.
    {
        float const det = cfg->pad_detune;
        uint32_t const inc_a = audio_dsp_phase_inc(m->pad_freq_a * (1.0f - det));
        uint32_t const inc_b = audio_dsp_phase_inc(m->pad_freq_b);
        uint32_t const inc_c = audio_dsp_phase_inc(m->pad_freq_c * (1.0f + det));
        float const e   = audio_env_tick(&m->pad_env);
        float const lfo = audio_dsp_sin(m->pad_lfo_phase);
        float       s   = (audio_dsp_saw(m->pad_phase_a)
                        + audio_dsp_saw(m->pad_phase_b)
                        + audio_dsp_saw(m->pad_phase_c)) * (1.0f / 3.0f);
        s = audio_biquad_tick(&m->pad_lpf, s);
        s *= (0.8f + 0.2f * lfo);
        m->pad_phase_a += inc_a;
        m->pad_phase_b += inc_b;
        m->pad_phase_c += inc_c;
        m->pad_lfo_phase += audio_dsp_phase_inc(cfg->pad_lfo_hz);
        out += s * e * cfg->pad_amp;
    }

    // Kick — sine with a fast pitch drop synthesised from the envelope.
    {
        float const e = audio_env_tick(&m->kick_env);
        float const f = 40.0f + e * 60.0f;
        uint32_t const inc = audio_dsp_phase_inc(f);
        float s = audio_dsp_sin(m->kick_phase);
        m->kick_phase += inc;
        out += s * e * cfg->kick_amp;
    }

    // Snare — bandpassed noise.
    {
        float const e = audio_env_tick(&m->snare_env);
        float n = audio_dsp_noise(&m->snare_noise);
        n = audio_biquad_tick(&m->snare_bpf, n);
        out += n * e * cfg->snare_amp;
    }

    // Hi-hat — highpassed noise.
    {
        float const e = audio_env_tick(&m->hat_env);
        float n = audio_dsp_noise(&m->hat_noise);
        n = audio_biquad_tick(&m->hat_hpf, n);
        out += n * e * cfg->hat_amp;
    }

    return out;
}

static void music_render(music_source_t* self, int16_t* out, size_t frames) {
    music_proc_t* m = (music_proc_t*)self;

    for (size_t i = 0; i < frames; i++) {
        // Tick scheduler.
        if (m->sample_pos >= m->next_tick_at) {
            on_tick(m);
            m->next_tick_at += m->samples_per_16th;
        }

        float const s = render_sample(m);
        int16_t const sample = audio_dsp_to_s16(s);
        out[2 * i + 0] = sample;
        out[2 * i + 1] = sample;

        m->sample_pos += 1.0;
    }
}

// ---------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------

static uint32_t hash32(uint32_t x, char const* tag) {
    // Mix the seed with a stable per-purpose tag so two namespaces
    // ("world", "music") never coincide.
    uint32_t h = x ^ 0xA5A55A5Au;
    for (char const* p = tag; *p; p++) {
        h ^= (uint32_t)(unsigned char)*p;
        h *= 0x01000193u;
    }
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    if (h == 0) h = 1;
    return h;
}

static void music_reseed(music_proc_t* m, uint32_t seed) {
    se_music_config_t const* cfg = m->cfg;
    m->prng = hash32(seed, "music");

    // Pick the run's tonic from the config's pool.
    m->root_midi = cfg->tonics[prng_range(&m->prng, (uint32_t)cfg->tonic_count)];

    // Per-run tempo — an integer BPM in [bpm_min, bpm_min+bpm_span) so no
    // two seeds feel metronome-identical (span 0 => fixed tempo).
    {
        uint32_t const bpm = cfg->bpm_min + prng_range(&m->prng, cfg->bpm_span);
        m->samples_per_16th =
            (double)AUDIO_SAMPLE_RATE_HZ * 60.0 / ((double)bpm * 4.0);
    }

    // Force a fresh section setup (sets prog, drums, arp, bass,
    // layers). arp_index starts at an impossible value so the
    // first section's no-repeat check can't false-match.
    memset(&m->prog, 0, sizeof(m->prog));
    m->drums       = cfg->drum_patterns[0];
    m->arp_index   = 0xFFu;
    m->layer_bass  = true;
    m->layer_arp   = true;
    m->layer_pad   = true;
    m->layer_drums = true;
    m->section_index = 0;
    start_new_section(m);
    m->section_index = 0;  // start_new_section bumped it — reset

    // Reset timing.
    m->sample_pos    = 0.0;
    m->next_tick_at  = 0.0;
    m->tick_in_bar   = 0;
    m->bar_in_section= 0;
    m->pad_last_chord_index = -1;

    // Reset all envelopes so a fresh seed doesn't leak the old
    // run's voice tails.
    audio_env_reset(&m->bass_env);
    audio_env_reset(&m->arp_env);
    audio_env_reset(&m->pad_env);
    audio_env_reset(&m->kick_env);
    audio_env_reset(&m->snare_env);
    audio_env_reset(&m->hat_env);
    audio_biquad_reset(&m->bass_lpf);
    audio_biquad_reset(&m->arp_lpf);
    audio_biquad_reset(&m->pad_lpf);
    audio_biquad_reset(&m->snare_bpf);
    audio_biquad_reset(&m->hat_hpf);
}

static void music_on_seed(music_source_t* self, uint32_t seed) {
    music_reseed((music_proc_t*)self, seed);
}

static void music_shutdown(music_source_t* self) {
    heap_caps_free(self);
}

// True only if every pattern bank is present and non-empty.
static bool config_valid(se_music_config_t const* c) {
    return c->progressions  && c->progression_count >= 1
        && c->arp_patterns   && c->arp_pattern_count  >= 1
        && c->drum_patterns  && c->drum_pattern_count >= 1
        && c->bass_patterns  && c->bass_pattern_count >= 1
        && c->tonics         && c->tonic_count        >= 1;
}

music_source_t* music_procedural_create(se_music_config_t const* cfg, uint32_t seed) {
    if (cfg == NULL) {
        cfg = &g_synthwave_preset;
    } else if (!config_valid(cfg)) {
        ESP_LOGW(TAG, "music config has an empty/NULL bank; using synthwave preset");
        cfg = &g_synthwave_preset;
    }

    music_proc_t* m = (music_proc_t*)heap_caps_calloc(1, sizeof(*m), MALLOC_CAP_INTERNAL);
    if (m == NULL) {
        ESP_LOGE(TAG, "alloc failed");
        return NULL;
    }

    m->cfg           = cfg;
    m->base.render   = music_render;
    m->base.on_seed  = music_on_seed;
    m->base.shutdown = music_shutdown;

    // Envelope shapes + filters for each voice come from the config.
    audio_env_configure(&m->bass_env,  cfg->bass_env.attack,  cfg->bass_env.decay,  cfg->bass_env.sustain,  cfg->bass_env.release);
    audio_env_configure(&m->arp_env,   cfg->arp_env.attack,   cfg->arp_env.decay,   cfg->arp_env.sustain,   cfg->arp_env.release);
    audio_env_configure(&m->pad_env,   cfg->pad_env.attack,   cfg->pad_env.decay,   cfg->pad_env.sustain,   cfg->pad_env.release);
    audio_env_configure(&m->kick_env,  cfg->kick_env.attack,  cfg->kick_env.decay,  cfg->kick_env.sustain,  cfg->kick_env.release);
    audio_env_configure(&m->snare_env, cfg->snare_env.attack, cfg->snare_env.decay, cfg->snare_env.sustain, cfg->snare_env.release);
    audio_env_configure(&m->hat_env,   cfg->hat_env.attack,   cfg->hat_env.decay,   cfg->hat_env.sustain,   cfg->hat_env.release);

    audio_biquad_lpf(&m->bass_lpf,  cfg->bass_lpf.hz,  cfg->bass_lpf.q);
    audio_biquad_lpf(&m->arp_lpf,   cfg->arp_lpf.hz,   cfg->arp_lpf.q);
    audio_biquad_lpf(&m->pad_lpf,   cfg->pad_lpf.hz,   cfg->pad_lpf.q);
    audio_biquad_bpf(&m->snare_bpf, cfg->snare_bpf.hz, cfg->snare_bpf.q);
    audio_biquad_hpf(&m->hat_hpf,   cfg->hat_hpf.hz,   cfg->hat_hpf.q);

    music_reseed(m, seed);

    ESP_LOGI(TAG, "music_procedural_create: seed=%u tonic_midi=%d", (unsigned)seed, m->root_midi);
    return &m->base;
}
