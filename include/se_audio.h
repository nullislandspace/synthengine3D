// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  audio mixer
// ---------------------------------------------------------------------
//  Part of the semver'd public surface (see se_version.h). The mixer
//  carries no app/NVS dependency: the host pushes per-class output gates
//  via audio_mixer_set_*_enabled() rather than the mixer reading app
//  settings. Implement music with se_audio_source.h's music_source_t
//  (se_music_procedural.h is one ready source); SFX with sfx_voice_t.
// =====================================================================
//
// Software audio mixer owning the BSP's single I2S channel.
//
// Two kinds of source: one music slot (typically a procedural
// generator today; future modplayer / MP3 / MIDI sources will
// plug into the same slot) and N SFX voices for short-lived
// effects (engine hum, ding, crash, scrape, cube-bump, …).
//
// Pipeline format: 22050 Hz, signed-16-bit PCM, stereo L/R
// interleaved. See `se_audio_source.h` for the source contract.
//
// Idle power management: when the music slot is NULL and every
// SFX voice is finished, the mixer keeps feeding silence to the
// I2S DMA queue for a short drain window (~46 ms), then mutes
// the speaker amplifier and disables the I2S channel; it blocks
// until a producer pokes it via `audio_mixer_set_music()` or
// `audio_mixer_register_voice()`.

#pragma once

#include "se_audio_source.h"
#include "esp_err.h"
#include <stdbool.h>

// Initialise BSP audio at AUDIO_SAMPLE_RATE_HZ, take the I2S
// channel, start the mixer task. Idempotent.
esp_err_t audio_mixer_init(void);

// Synchronous shutdown: mute the amplifier and disable the I2S
// channel right now. Call before `bsp_device_restart_to_launcher()`
// so the speaker doesn't sit on residual DMA samples. After this
// the mixer task remains alive but parked — calling
// `audio_mixer_set_music()` or `_register_voice()` after this is
// a no-op (the speaker stays muted) by design.
void audio_mixer_shutdown(void);

// Install the music source. NULL clears the slot. The previous
// source (if any) has its `shutdown()` callback fired and is then
// forgotten — callers must not retain the pointer they passed in.
void audio_mixer_set_music(music_source_t* src);

// Register an SFX voice with the mixer. The voice's storage must
// remain valid until the voice's `finished` flag is set (one-shot
// or owner-controlled) — the mixer takes a copy of the pointer,
// not the struct contents. Returns true on success, false if all
// SFX slots are full.
bool audio_mixer_register_voice(sfx_voice_t* v);

// Mark a voice as finished so the mixer reaps it on the next tick.
// Safe to call after the voice has already been reaped; idempotent.
void audio_mixer_stop_voice(sfx_voice_t* v);

// Stop *all* voices. Used on game over / leaving the playing state
// when persistent effects (engine hum, scrape) should end. Does
// not affect the music slot.
void audio_mixer_stop_all_voices(void);

// Output gates pushed by the host app (at startup and whenever the
// player toggles a setting) so the mixer can mute output without the
// engine knowing where the app stores its preferences or what its mute
// categories mean. `set_music_enabled` gates the single music slot;
// `set_group_enabled` gates all SFX voices whose `group` matches (see
// sfx_voice_t.group — the group's meaning is the app's choice). Valid
// groups are [0, SE_AUDIO_SFX_GROUP_COUNT); out-of-range is ignored.
// All gates default to enabled. Safe to call before audio_mixer_init().
void audio_mixer_set_music_enabled(bool on);
void audio_mixer_set_group_enabled(uint8_t group, bool on);
