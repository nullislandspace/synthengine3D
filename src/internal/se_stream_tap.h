#pragma once
// =====================================================================
//  se_stream_tap  --  one line of audio_mixer.c's business with the
//  streamer, and nothing more
// ---------------------------------------------------------------------
//  The mixer must not know what a stream is, and se_stream.c must not
//  reach into the mixer. This is the whole of the seam between them: the
//  mixer offers every chunk it writes to the I2S, silence included, and
//  what happens to it is not the mixer's concern.
//
//  IT IS CALLED FROM THE MIXER TASK, which is feeding a DMA on a
//  deadline, so the implementation copies and returns. It is a no-op
//  when nothing is streaming.
// =====================================================================

#include <stddef.h>
#include <stdint.h>

// `frames` interleaved stereo int16 at AUDIO_SAMPLE_RATE_HZ.
void se_stream_tap(int16_t const* pcm, size_t frames);
