#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  MP3 music source
// ---------------------------------------------------------------------
//  Plays MP3 files from a directory as the mixer's music source, so a
//  game can offer "my own music" as an alternative to the procedural
//  generator (se_music_procedural.h) with no other change: both are
//  music_source_t, and the mixer neither knows nor cares which it holds.
//
//      music_source_t* m = se_mp3_create(NULL);   // NULL -> "/sd/music"
//      if (m) audio_mixer_set_music(m);           // else keep procedural
//
//  Decoding is minimp3 (public domain), vendored in src/internal/.
//
//  HOW IT MEETS THE MIXER'S CONTRACT. se_audio_source.h forbids blocking
//  in render() -- no file I/O, no waits -- and fixes the format at
//  22050 Hz stereo. An MP3 satisfies neither on its own: it lives on the
//  SD card and is usually 44.1 kHz. So this source splits in two:
//
//    * a decoder TASK reads the file, runs minimp3, resamples to
//      22050 Hz stereo and fills a ring buffer;
//    * render() only drains that ring buffer -- a bounded memcpy that
//      never touches the filesystem.
//
//  If the decoder falls behind (a slow SD read, a busy frame), render()
//  emits silence for the shortfall rather than stalling the mixer. You
//  hear a dropout; the audio path never blocks.
//
//  COST, because it is not free: a decoder task with a 32 KB stack
//  (minimp3 is stack-hungry), a ~64 KB PCM ring and a 16 KB read buffer
//  in PSRAM, plus the CPU to decode. The task is pinned to the mixer's
//  core below the mixer's priority, so it yields to audio, but it is
//  real work a procedural source does not do.
// =====================================================================

#include <stdbool.h>

#include "se_audio_source.h"   // music_source_t

typedef struct {
    // Directory to scan (non-recursively) for *.mp3. NULL -> "/sd/music".
    char const* dir;
    // Play the playlist in a random order. Default false (alphabetical,
    // as readdir returns them sorted by the engine).
    bool shuffle;
    // Restart the playlist after the last track. Default true; when
    // false the source goes silent once the last track ends.
    bool loop;
} se_mp3_config_t;

// Build an MP3 music source over the configured directory. `cfg` may be
// NULL for all defaults.
//
// Returns NULL -- having logged why -- if the directory is missing, holds
// no .mp3 files, or the buffers/task could not be allocated. A game
// should treat NULL as "MP3 not available" and keep whatever music
// source it already had, rather than ending up silent.
//
// Ownership passes to the mixer: audio_mixer_set_music() takes it, and
// releases it (stopping the decoder task and freeing the buffers) when
// the music source is replaced or the mixer shuts down. Do not free it
// yourself.
music_source_t* se_mp3_create(se_mp3_config_t const* cfg);

// Number of tracks found. 0 if `src` is not an MP3 source.
int se_mp3_track_count(music_source_t* src);

// Filename (not the full path) of the track now playing, or "" if none.
// The storage belongs to the source and stays valid until it is replaced.
char const* se_mp3_track_name(music_source_t* src);

// Skip to the next track. Takes effect within a few audio chunks.
void se_mp3_skip(music_source_t* src);
