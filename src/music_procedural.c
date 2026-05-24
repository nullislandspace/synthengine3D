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

// The synthwave kick's fundamental; its pitch envelope sweeps up from
// here by the kick voice's pitch_env_amt and decays back down.
#define KICK_BASE_HZ       40.0f

// Sample block between scheduler ticks: voices render in chunks of at most
// this many samples (one indirect call per voice per chunk). A 16th note
// is ~3000 samples at these tempos, so chunks are tick-bounded, never
// crossing a note boundary.
#define MUSIC_BLOCK        32

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

// Per-role voices. Each spec is the built-in subtractive/noise voice.
// The pad's gain is its layer gain split across the three chord-tone
// voices (0.14 / 3) so the summed pad matches the old single-block level;
// the others carry their full layer gain. Sum stays well under 1.0 before
// the mixer's ~30% music pass.
static se_music_config_t const g_synthwave_preset = {
    .bpm_min = 100u, .bpm_span = 19u,              // 100..118 BPM
    .tonics = g_synth_tonics, .tonic_count = ARRAY_LEN(g_synth_tonics),
    .progressions  = g_synth_progressions, .progression_count = ARRAY_LEN(g_synth_progressions),
    .arp_patterns  = g_synth_arps,         .arp_pattern_count  = ARRAY_LEN(g_synth_arps),
    .drum_patterns = g_synth_drums,        .drum_pattern_count = ARRAY_LEN(g_synth_drums),
    .bass_patterns = g_synth_bass,         .bass_pattern_count = ARRAY_LEN(g_synth_bass),
    // Bass: dark saw. Arp: bright square. Pad: warm saw with a slow amp LFO.
    .bass  = { .osc = SE_OSC_SAW,    .osc_count = 1, .env = { 0.005f, 0.18f, 0.5f, 0.06f },
               .filter = SE_FILTER_LPF, .cutoff_hz = 600.0f,  .q = 0.9f, .gain = 0.30f },
    .arp   = { .osc = SE_OSC_SQUARE, .osc_count = 1, .env = { 0.002f, 0.08f, 0.0f, 0.04f },
               .filter = SE_FILTER_LPF, .cutoff_hz = 2400.0f, .q = 0.7f, .gain = 0.16f },
    .pad   = { .osc = SE_OSC_SAW,    .osc_count = 1, .env = { 0.40f, 0.30f, 0.7f, 1.20f },
               .filter = SE_FILTER_LPF, .cutoff_hz = 1200.0f, .q = 0.7f, .gain = 0.14f / 3.0f,
               .amp_lfo_hz = 0.4f, .amp_lfo_depth = 0.2f },
    // Kick: sine with a fast pitch drop (40 Hz + 60 Hz * env). No filter.
    .kick  = { .osc = SE_OSC_SINE,   .osc_count = 1, .env = { 0.001f, 0.08f, 0.0f, 0.001f },
               .filter = SE_FILTER_NONE, .gain = 0.45f, .pitch_env_amt = 60.0f },
    // Snare: bandpassed noise. Hat: highpassed noise.
    .snare = { .osc = SE_OSC_NOISE,  .osc_count = 1, .env = { 0.001f, 0.08f, 0.0f, 0.001f },
               .filter = SE_FILTER_BPF, .cutoff_hz = 1800.0f, .q = 0.9f, .gain = 0.32f },
    .hat   = { .osc = SE_OSC_NOISE,  .osc_count = 1, .env = { 0.001f, 0.03f, 0.0f, 0.001f },
               .filter = SE_FILTER_HPF, .cutoff_hz = 7000.0f, .q = 0.8f, .gain = 0.10f },
    .pad_detune = 0.005f,
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
    se_music_arp_pattern_t  arp_pat;        // this section's arp pattern
    uint8_t        arp_index;      // its index in the arp bank (no-repeat)
    int            arp_octave;     // 0 or +12 — per-section arp transpose
    uint16_t       bass_mask;      // this section's bass rhythm
    bool           layer_bass;
    bool           layer_arp;
    bool           layer_pad;
    bool           layer_drums;
    int8_t         pad_last_chord_index;

    // Built-in voices, used for any role the config doesn't override. The
    // pad is polyphonic: three voices for the chord's root / third / fifth.
    se_voice_synth_t v_bass, v_arp, v_pad[3], v_kick, v_snare, v_hat;

    // The voice actually played for each role — a built-in above or a
    // config-supplied custom se_voice_t. Set once at create.
    se_voice_t* bass;
    se_voice_t* arp;
    se_voice_t* pad[3];
    se_voice_t* kick;
    se_voice_t* snare;
    se_voice_t* hat;
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
        m->arp_pat   = cfg->arp_patterns[idx];
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
            m->bass->note_on(m->bass, midi_to_hz(bass_midi), 1.0f);
        }
    }

    // Arp: walk the section's arp pattern. A negative step is a
    // rest (no trigger); the rest are chord tones lifted into
    // octaves, optionally transposed up an octave for the section.
    if (m->layer_arp) {
        int const step = m->arp_pat.step[m->tick_in_bar];
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
            m->arp->note_on(m->arp, midi_to_hz(midi + m->arp_octave), 1.0f);
        }
    }

    // Pad: retrigger on chord changes only. Three voices play the chord's
    // root / third / fifth; the outer two are detuned for width.
    int8_t const chord_index = (int8_t)((m->bar_in_section / BARS_PER_CHORD) % CHORDS_PER_SECTION);
    if (m->layer_pad && chord_index != m->pad_last_chord_index) {
        float const det = m->cfg->pad_detune;
        m->pad[0]->note_on(m->pad[0], midi_to_hz(tones[0]) * (1.0f - det), 1.0f);
        m->pad[1]->note_on(m->pad[1], midi_to_hz(tones[1]),                1.0f);
        m->pad[2]->note_on(m->pad[2], midi_to_hz(tones[2]) * (1.0f + det), 1.0f);
        m->pad_last_chord_index = chord_index;
    }

    // Drums. The kick's pitch comes from its voice (KICK_BASE_HZ + its
    // pitch envelope); snare / hat are noise, so their pitch is ignored.
    if (m->layer_drums) {
        uint16_t const bit = (uint16_t)(1u << m->tick_in_bar);
        if (m->drums.kick  & bit) m->kick->note_on(m->kick,   KICK_BASE_HZ, 1.0f);
        if (m->drums.snare & bit) m->snare->note_on(m->snare, 0.0f,         1.0f);
        if (m->drums.hat   & bit) m->hat->note_on(m->hat,     0.0f,         1.0f);
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

// Add one voice's output into the mono accumulator, skipping it when it
// is silent (envelope idle) so a quiet layer costs nothing.
static inline void mix_voice(se_voice_t* v, float* accum, size_t n) {
    if (v->active && !v->active(v)) return;
    v->render(v, accum, n);
}

static void music_render(music_source_t* self, int16_t* out, size_t frames) {
    music_proc_t* m = (music_proc_t*)self;

    size_t done = 0;
    while (done < frames) {
        // Fire any 16th-note tick that is due, then continue (so several
        // due ticks — never the case at these tempos — would all fire).
        if (m->sample_pos >= m->next_tick_at) {
            on_tick(m);
            m->next_tick_at += m->samples_per_16th;
            continue;
        }

        // Render up to the next tick, bounded by the scratch block and the
        // frames remaining. `until` is fractional; flooring it keeps ticks
        // firing on the same integer sample they did sample-by-sample.
        double const until = m->next_tick_at - m->sample_pos;
        size_t n = frames - done;
        if ((double)n > until) {
            size_t const u = (size_t)until;
            n = (u < 1) ? 1 : u;
        }
        if (n > MUSIC_BLOCK) n = MUSIC_BLOCK;

        float accum[MUSIC_BLOCK];
        for (size_t k = 0; k < n; k++) accum[k] = 0.0f;

        mix_voice(m->bass,   accum, n);
        mix_voice(m->arp,    accum, n);
        mix_voice(m->pad[0], accum, n);
        mix_voice(m->pad[1], accum, n);
        mix_voice(m->pad[2], accum, n);
        mix_voice(m->kick,   accum, n);
        mix_voice(m->snare,  accum, n);
        mix_voice(m->hat,    accum, n);

        for (size_t k = 0; k < n; k++) {
            int16_t const sample = audio_dsp_to_s16(accum[k]);
            out[2 * (done + k) + 0] = sample;
            out[2 * (done + k) + 1] = sample;
        }

        m->sample_pos += (double)n;
        done          += n;
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

// Re-initialise the built-in voices from their specs — a full reset that
// silences voice tails. Custom (config-supplied) voices are caller-owned
// and left untouched. Role pointers, set once in create, stay valid (the
// embedded structs are reset in place).
static void reinit_builtin_voices(music_proc_t* m) {
    se_music_config_t const* cfg = m->cfg;
    se_voice_synth_init(&m->v_bass,    &cfg->bass);
    se_voice_synth_init(&m->v_arp,     &cfg->arp);
    se_voice_synth_init(&m->v_pad[0],  &cfg->pad);
    se_voice_synth_init(&m->v_pad[1],  &cfg->pad);
    se_voice_synth_init(&m->v_pad[2],  &cfg->pad);
    se_voice_synth_init(&m->v_kick,    &cfg->kick);
    se_voice_synth_init(&m->v_snare,   &cfg->snare);
    se_voice_synth_init(&m->v_hat,     &cfg->hat);
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

    // Reset the voices so a fresh seed doesn't leak the old run's tails.
    reinit_builtin_voices(m);
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

    // Build the built-in voices from the per-role specs, then route each
    // role to its custom override (if any) or its built-in. The pad is
    // polyphonic, so it always uses its three built-in voices.
    reinit_builtin_voices(m);
    m->bass   = cfg->bass_voice  ? cfg->bass_voice  : &m->v_bass.base;
    m->arp    = cfg->arp_voice   ? cfg->arp_voice   : &m->v_arp.base;
    m->pad[0] = &m->v_pad[0].base;
    m->pad[1] = &m->v_pad[1].base;
    m->pad[2] = &m->v_pad[2].base;
    m->kick   = cfg->kick_voice  ? cfg->kick_voice  : &m->v_kick.base;
    m->snare  = cfg->snare_voice ? cfg->snare_voice : &m->v_snare.base;
    m->hat    = cfg->hat_voice   ? cfg->hat_voice   : &m->v_hat.base;

    music_reseed(m, seed);

    ESP_LOGI(TAG, "music_procedural_create: seed=%u tonic_midi=%d", (unsigned)seed, m->root_midi);
    return &m->base;
}
