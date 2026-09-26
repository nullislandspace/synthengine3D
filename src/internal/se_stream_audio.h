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
//       22050) and an MPEG audio frame is 1152, so they do not line up
//       and something has to hold the remainder;
//    3. the ENCODER, run by the stream task, which turns whole frames
//       into MPEG audio and stamps them with a PTS taken from the
//       SAMPLE COUNT -- not from a clock. Audio that is timed by
//       counting what was actually played cannot drift against itself.
//
//  THE MIXER RUNS AT 22050 Hz, STEREO (AUDIO_SAMPLE_RATE_HZ). Whether
//  that is what goes out depends on the codec: MPEG-1 Layer II wants
//  32/44.1/48 kHz, so it would need a x2 upsample (exact, 22050*2 =
//  44100); MPEG-2 Layer III takes 22050 as it is.
//
//  WHY THERE IS NO CODEC HERE YET. Every MPEG audio encoder worth
//  vendoring -- shine, twolame, lame -- is LGPL, and everything this
//  engine vendors so far is permissive (minimp3 is public domain,
//  TinyUSB is MIT). Writing a conforming one instead means the 512-tap
//  analysis window and the quantisation tables, which are not the sort
//  of thing to reproduce from memory: wrong, they decode as plausible
//  noise. So the plumbing is here and the codec is a decision (D-96).
//  Until it is made, se_stream_audio_prepare() answers honestly and the
//  stream carries video alone.
// =====================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Buffers for the ring. False if they would not fit, or if there is no
// encoder compiled in -- the caller then streams video only.
bool se_stream_audio_prepare(void);
void se_stream_audio_free(void);

// From the MIXER TASK: `n` frames of interleaved stereo int16, as they
// went to the I2S. False if the ring is full (counted by the caller as
// a drop; the stream skips, the game is not held up).
bool se_stream_audio_push(int16_t const* frames, size_t n);

// From the STREAM TASK: the next encoded frame, if a whole one is
// ready. `pts` is in 90 kHz ticks, from the sample count.
bool se_stream_audio_take(uint8_t const** data, size_t* len, uint64_t* pts);
