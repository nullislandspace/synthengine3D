# Audio (`se_audio.h`, `se_audio_source.h`, `se_audio_dsp.h`, `se_music_procedural.h`)

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

## Procedural music (`se_music_procedural.h`)

A ready-made `music_source_t` driven by a **config + seed**:

```c
music_source_t* music_procedural_create(se_music_config_t const* cfg, uint32_t seed);

audio_mixer_set_music(music_procedural_create(NULL, seed));  // NULL = synthwave preset
```

Seed-derived — the same config + seed always yields the same music, from a
PRNG separate from any world generator so toggling music never perturbs other
seed-driven content.

### The config is the content; the synth is the engine

The generator's **structure is fixed** — a six-voice synth (saw bass, square
arp, three-saw pad, sine kick, noise snare, noise hi-hat) on a 4/4 16th-note
grid, in 16-bar sections of eight two-bar chords. Its **content and tone are
data**, supplied as `se_music_config_t`:

- **tempo** — `bpm_min` + `bpm_span` (a BPM picked per run in the range);
- **key** — `tonics[]` (a MIDI-note pool, one chosen per run);
- **harmony / rhythm banks** — `progressions[]`, `arp_patterns[]`,
  `drum_patterns[]`, `bass_patterns[]` (a fresh selection per section);
- **balance** — per-layer gains (`bass_amp` … `hat_amp`);
- **timbre** — each voice's `se_music_env_t` envelope + `se_music_filter_t`,
  plus the pad's detune + LFO rate.

So the same generator plays genuinely different music (different key, rhythm,
harmony, balance, timbre) with no code change — only the synth topology is
shared. (Making the voice *waveforms* pluggable too is a possible future step.)

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
    /* arp/drum/bass banks + gains + per-voice env/filter … */
};
music_procedural_create(&MY_MUSIC, seed);   // your music, same generator
```

`se_music_synthwave_preset()` returns the built-in synthwave config (what
`NULL` selects). The config + its bank arrays are **retained by reference** —
point them at static storage that outlives the source. See the header for the
full struct + the grid constants (`SE_MUSIC_TICKS_PER_BAR`,
`SE_MUSIC_CHORDS_PER_SECTION`) the bank shapes use.
