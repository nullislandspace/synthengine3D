#pragma once
// =====================================================================
//  se_stream_audio  --  the mixer's output, on its way into the stream
// ---------------------------------------------------------------------
//  Internal to se_stream.c. Three jobs, in the order the samples meet
//  them:
//
//    1. the TAP. audio_mixer.c hands over every chunk it writes to I2S,
//       from the mixer task. That is a real-time task feeding a DMA, so
//       this copies and returns -- it never encodes there;
//    2. the RING, because a mixer chunk is 256 frames (~11.6 ms at
//       22050) and a Layer II frame is 1152 (~52 ms), so they do not
//       line up and something has to hold the remainder;
//    3. the ENCODER, run by the stream task, which turns whole frames
//       into MPEG audio and stamps them with a PTS taken from the
//       SAMPLE COUNT -- not from a clock. Audio that is timed by
//       counting what was actually played cannot drift against itself.
//
//  THE MIXER RUNS AT 22050 Hz, STEREO (AUDIO_SAMPLE_RATE_HZ), AND THAT
//  IS WHAT GOES OUT. 22050 is an MPEG-2 LSF rate and LSF Layer II takes
//  it directly, so nothing is resampled between the speaker and the
//  stream -- and LSF Layer II has exactly one bit allocation table,
//  so there is no rate-dependent table selection to get wrong.
//
//  THE CODEC IS PUBLIC DOMAIN, AND THAT TOOK WRITING ONE. Every MPEG
//  audio encoder worth vendoring -- shine, twolame, lame -- is LGPL,
//  and everything else this engine vendors is permissive (minimp3 is
//  CC0, TinyUSB is MIT). An app.so that ships as a single blob nobody
//  can relink is close to the worst case for LGPL section 6, so the
//  encoder was written instead: see pdmp2/PROVENANCE.md for where every
//  table in it came from, and why the filterbank window had to be
//  measured rather than designed.
// =====================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Buffers for the ring. False if they would not fit, or if there is no
// encoder compiled in -- the caller then streams video only.
bool se_stream_audio_prepare(void);
void se_stream_audio_free(void);

// THROW AWAY WHATEVER IS QUEUED AND RESTART THE CLOCK. Call it at the
// instant the consumer starts, which is not the instant prepare() ran:
// bringing the USB link up takes time, the mixer is already filling the
// ring throughout, and the first frame taken out is therefore that stale
// while being stamped as current. That is a CONSTANT audio delay of
// however long the gap was -- measured at about 850 ms on this badge,
// which is enough for OBS to give up buffering and restart its source
// audio over and over.
void se_stream_audio_reset(void);

// From the MIXER TASK: `n` frames of interleaved stereo int16, as they
// went to the I2S. Never blocks and never refuses: if the consumer has
// fallen behind, this OVERWRITES what it has not read yet, and the
// consumer accounts for the loss so the audio clock stays true. Refusing
// instead -- which is what this used to do -- shortens the audio timeline
// by every dropped chunk, and that error accumulates without bound.
bool se_stream_audio_push(int16_t const* frames, size_t n);

// Frames of PCM the writer overwrote before the consumer read them. Each
// one is a gap in the sound AND a sample the timeline accounted for
// anyway; a number that keeps climbing means the ring is too shallow or
// the stream task is not getting to run.
uint64_t se_stream_audio_lost(void);

// From the STREAM TASK: the next encoded frame, if a whole one is
// ready. `pts` is in 90 kHz ticks, from the sample count.
bool se_stream_audio_take(uint8_t const** data, size_t* len, uint64_t* pts);

// The cost of se_stream_audio_take(), split into the ring copy and the
// codec. Cumulative microseconds since prepare(); see pdmp2_port.h.
void se_stream_audio_cost(uint64_t* copy_us, uint64_t* enc_us, uint32_t* n);

// Cumulative microseconds in each phase of the codec, and the frames they
// cover. Zero when no codec is compiled in. See pdmp2.h.
void se_stream_audio_phases(uint64_t* an, uint64_t* scf, uint64_t* alloc, uint64_t* wr,
                            uint32_t* n);
