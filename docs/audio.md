# Audio (`se_audio.h`, `se_audio_source.h`, `se_audio_dsp.h`, `se_voice.h`, `se_music_procedural.h`)

A software mixer that owns the BSP's single I2S channel and sums one **music**
source plus N **SFX voices**. The pipeline is **22050 Hz, signed-16-bit,
stereo (L/R interleaved)** end to end.

## The mixer

`se_run` calls `audio_mixer_init()` at boot (idempotent). The mixer runs on its
own task; the game only hands it sources and pushes mute gates.

```c
esp_err_t audio_mixer_init(void);          // boot (engine does this)
void audio_mixer_shutdown(void);           // mute + stop now (before reboot)

void audio_mixer_set_music(music_source_t* src);     // NULL clears the slot
bool audio_mixer_register_voice(sfx_voice_t* v);     // false if all slots full
void audio_mixer_stop_voice(sfx_voice_t* v);
void audio_mixer_stop_all_voices(void);

void audio_mixer_set_music_enabled(bool on);            // gate the music slot
void audio_mixer_set_group_enabled(uint8_t group, bool on);  // gate an SFX group
```

**Idle power-down:** when the music slot is NULL and every voice is finished,
the mixer feeds silence for a short drain window (~46 ms), then mutes the
speaker amp and disables I2S until a producer pokes it. So "no sound" really
means no power draw — you don't manage that.

**Mute gates are pushed, not pulled.** The mixer never reads your settings; the
host calls `set_music_enabled` / `set_group_enabled` at startup and whenever a
toggle changes. SFX voices carry a `group` index ( `[0, SE_AUDIO_SFX_GROUP_COUNT)` );
the *meaning* of each group is yours (e.g. group 0 general, group 1 a
persistent engine drone with its own mute). All gates default to enabled.

## Source contracts

The mixer only sees two trait structs (in `se_audio_source.h`). You implement
them by **embedding** the struct in your own state so the function pointers and
flags are the entire contract — no registration of types, no allocation rules
beyond "stay alive while in use".

### `music_source_t` — the one music slot

```c
struct music_source_s {
    void (*render)(music_source_t* self, int16_t* stereo_out, size_t frames);
    void (*on_seed)(music_source_t* self, uint32_t seed);   // optional
    void (*shutdown)(music_source_t* self);                 // optional
    // your state follows
};
```

`render` writes exactly `frames` stereo frames (`frames * 2` int16) to a
pre-zeroed buffer; the mixer scales by the music gain (`AUDIO_MUSIC_GAIN`).
`audio_mixer_set_music(NULL)` (or replacing the source, or shutdown) fires
`shutdown()` and forgets the pointer — so the source can free itself there.

### `sfx_voice_t` — short or persistent effects

```c
struct sfx_voice_s {
    void (*render)(sfx_voice_t* self, int16_t* stereo_out, size_t frames);
    void (*shutdown)(sfx_voice_t* self);   // optional
    bool    finished;                       // set true when done -> mixer reaps
    uint8_t group;                          // mute group (app-defined meaning)
    // your state follows
};
```

`render` sums into the pre-zeroed buffer at unity gain (scaled by
`AUDIO_SFX_GAIN`). A one-shot sets `finished = true` when it runs dry; a
persistent voice (a drone) keeps `finished` false and is stopped explicitly via
`audio_mixer_stop_voice()` / `audio_mixer_stop_all_voices()`.

**Callback rules (both kinds):** they run on the mixer task and **must not
block** — no FreeRTOS waits, no NVS, no file I/O, no WARN+ logging. Any state
the audio task reads must already be in place.

### Gain staging

`AUDIO_MUSIC_GAIN` / `AUDIO_SFX_GAIN` (in [`se_config.h`](configuration.md))
set the master music-vs-SFX balance; the mixer hard-clips the int16
accumulator. With several random-phase voices summing by ≈√N, the defaults
leave headroom for music + a few SFX without clipping — retune by ear, then
check the headroom on paper.

## DSP primitives (`se_audio_dsp.h`)

Building blocks for writing a source: oscillators, envelopes, a biquad filter.
Use them inside your `render` callbacks. (See the header for the exact set.)

## Voices (`se_voice.h`)

A **voice** is one playing note — the pluggable unit of synthesis. The
procedural generator assigns one voice per role; a future MIDI player will keep
a pool of them and route note-on/off events. Both see only the `se_voice_t`
vtable, so you can plug in your own synthesis or use the built-in one.

```c
struct se_voice_s {
    void (*note_on )(se_voice_t*, float freq_hz, float velocity); // trigger
    void (*note_off)(se_voice_t*);                                // release (may be NULL)
    void (*render  )(se_voice_t*, float* mix, size_t frames);     // ADD into mono mix
    bool (*active  )(se_voice_t const*);                          // still audible?
};
```

- **One voice = one note.** Chords and polyphony are *multiple voices*, never
  one voice playing several pitches — identical model for the sequencer and for
  MIDI.
- `render` **adds** into a mono float accumulator and runs its own inner loop,
  so it's one indirect call per voice per block, not per sample.

The built-in `se_voice_synth_t` is a configurable subtractive/noise voice
(`se_voice_spec_t`): oscillator (sine/saw/square/triangle/noise) ×1..N detuned →
optional filter (lpf/hpf/bpf) → ADSR → gain, plus an optional pitch envelope
(a kick's drop) and amplitude LFO (a pad's shimmer). Embed it, init in place,
no heap:

```c
static se_voice_synth_t lead;
se_voice_synth_init(&lead, &(se_voice_spec_t){
    .osc = SE_OSC_SAW, .osc_count = 3, .detune = 0.006f,   // a fat supersaw
    .env = { .attack = 0.01f, .decay = 0.2f, .sustain = 0.7f, .release = 0.3f },
    .filter = SE_FILTER_LPF, .cutoff_hz = 3000.0f, .q = 0.7f, .gain = 0.2f,
});
lead.base.note_on(&lead.base, 440.0f, 1.0f);   // play A4
```

## Procedural music (`se_music_procedural.h`)

A ready-made `music_source_t` driven by a **config + seed**:

```c
music_source_t* music_procedural_create(se_music_config_t const* cfg, uint32_t seed);

audio_mixer_set_music(music_procedural_create(NULL, seed));  // NULL = synthwave preset
```

Seed-derived — the same config + seed always yields the same music, from a
PRNG separate from any world generator so toggling music never perturbs other
seed-driven content.

### The config is the content; the arrangement is the engine

The generator's **arrangement is fixed** — six roles (bass, arp, pad, kick,
snare, hi-hat) on a 4/4 16th-note grid, in 16-bar sections of eight two-bar
chords. Everything else is **data**, supplied as `se_music_config_t`:

- **tempo** — `bpm_min` + `bpm_span` (a BPM picked per run in the range);
- **key** — `tonics[]` (a MIDI-note pool, one chosen per run);
- **harmony / rhythm banks** — `progressions[]`, `arp_patterns[]`,
  `drum_patterns[]`, `bass_patterns[]` (a fresh selection per section);
- **voices** — a `se_voice_spec_t` per role (`bass`, `arp`, `pad`, `kick`,
  `snare`, `hat`), each carrying its own oscillator, filter, envelope, **and
  gain** (the per-layer balance lives in each spec's `.gain`), plus the pad's
  `pad_detune` for chord width.

So the same generator plays genuinely different music — different key, rhythm,
harmony, balance, **and timbre** — with no code change. Because each role is a
voice, you can make the bass a square, the lead a supersaw, the kick a sine, and
so on, not just retune the existing waveforms.

For **full custom synthesis**, point a role's `*_voice` (e.g. `bass_voice`) at
your own `se_voice_t` and it's used instead of the spec. (The pad is polyphonic
— three voices — so it is spec-only.)

```c
static se_music_progression_t const MY_PROGS[]  = { /* {root_offset,is_major} × 8 */ };
static se_music_arp_pattern_t   const MY_ARPS[]  = { /* 16 steps; -1 = rest */ };
static se_music_drum_pattern_t  const MY_DRUMS[] = { /* kick/snare/hat 16-bit masks */ };
static uint16_t                 const MY_BASS[]  = { 0x5555u, /* … */ };
static int8_t                   const MY_KEYS[]  = { 48, 50, 53 };

static se_music_config_t const MY_MUSIC = {
    .bpm_min = 90, .bpm_span = 8,
    .tonics = MY_KEYS, .tonic_count = 3,
    .progressions = MY_PROGS, .progression_count = /* … */,
    /* arp/drum/bass banks … */
    .bass = { .osc = SE_OSC_SAW,   .osc_count = 1, .filter = SE_FILTER_LPF,
              .cutoff_hz = 600.0f, .q = 0.9f, .gain = 0.30f,
              .env = { 0.005f, 0.18f, 0.5f, 0.06f } },
    /* arp / pad / kick / snare / hat specs … */
};
music_procedural_create(&MY_MUSIC, seed);   // your music, same arrangement
```

`se_music_synthwave_preset()` returns the built-in synthwave config (what
`NULL` selects). The config + its bank arrays are **retained by reference** —
point them at static storage that outlives the source. See the header for the
full struct + the grid constants (`SE_MUSIC_TICKS_PER_BAR`,
`SE_MUSIC_CHORDS_PER_SECTION`) the bank shapes use.
