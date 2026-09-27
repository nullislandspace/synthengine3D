#pragma once
// =====================================================================
//  pdmp2_port  --  where the Layer II encoder's working set lives
// ---------------------------------------------------------------------
//  THE HOT 16.5 KB IN INTERNAL SRAM, THE OTHER 22 IN PSRAM, and both
//  numbers are measured (F-105, F-106). This header used to say:
//
//      "The encoder touches these buffers once per audio frame (52 ms),
//       so PSRAM's latency is irrelevant to it."
//
//  It touches them 184320 times per frame. The filterbank runs 36 slots x
//  2 channels and every pass reads all of `win`, all of `mat` and 512
//  floats of `work` -- 72 passes over the same 16.5 KB for one 1152-sample
//  frame. In PSRAM that cost 7 ms a frame while the cache happened to hold
//  it and 140 ms once the game's chunk meshing evicted it: the same code on
//  the same samples, twenty times slower, which is PSRAM miss latency
//  (38 ns an access against 760 ns). At 140 ms per 52 ms frame the stream
//  task cannot keep up by arithmetic, and the whole livestream collapsed to
//  0.7 fps with the encoder never looking slow on average.
//
//  BUT NOT ALL 38.5 KB, because internal RAM is shared with the thing the
//  audio exists to accompany. Taking the lot made `esp_h264_enc_open()`
//  fail, so the menu row would not tick at all -- audio protected at the
//  price of the video (F-106). `hist`, `sb` and `pcm` are touched about
//  twice a sample, so they stay in PSRAM where a streamed buffer belongs,
//  and only win (2.0 KB), mat (8.0) and work (6.5) come inside.
//
//  Every allocation falls back to PSRAM, so a badge short of internal RAM
//  gets a slow encoder rather than no sound -- and se_stream_start() now
//  takes the encoder's memory BEFORE the audio's, so a short heap can
//  never cost the picture again.
//
//  Reached by -DPDMP2_CONFIG_H='"pdmp2_port.h"' in CMakeLists.txt.
// =====================================================================

#include <stddef.h>

#include "esp_heap_caps.h"

#define PDMP2_MALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define PDMP2_FREE(p)   heap_caps_free((p))

// AND IT LEAVES A MARGIN, because "fall back when the allocation fails" is
// not enough: the USB network comes up AFTER the codec and wants internal
// DMA buffers of its own, so an encoder that succeeds by taking the last
// 20 KB kills the link instead of the picture -- the same mistake as F-106
// one step further down. Taking internal memory only while this much would
// still be left means the worst case is a slow encoder, which is a glitch,
// rather than a stream that will not start, which is a mystery.
// 48 KB, AND THE NUMBER IS ARITHMETIC RATHER THAN A ROUND GUESS. The guard
// is applied per allocation against a heap that is already shrinking, so a
// reserve that admits the first buffer can still refuse the third. Measured
// on this badge, 76667 B free before audio:
//
//   win  2048 -> needs 64K+2048  = 66560  ok, 74619 left
//   mat  8192 -> needs 64K+8192  = 72704  ok, 66427 left
//   work 6656 -> needs 64K+6656  = 70848  REFUSED, and work is read 36864
//                                         times a frame -- the fix missed
//                                         the buffer it most needed to hit
//
// All four (the three hot ones plus s_flat) fit only while the reserve is
// under 55163 B, so 48 KB clears it with room and leaves 55163 B for
// usbnet_start(), which comes after. The run that worked left usbnet 66427,
// so this trades 11 KB of its headroom for the buffer that matters.
#define PDMP2_PORT_INTERNAL_RESERVE (48u * 1024u)

static inline void* pdmp2_port_malloc_hot(size_t n) {
    size_t const free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (free_now > n + PDMP2_PORT_INTERNAL_RESERVE) {
        void* const p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p != NULL) return p;
    }
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
}

#define PDMP2_MALLOC_HOT(n) pdmp2_port_malloc_hot((n))

// Phase timing on, so pdmp2_profile_get() reports where a frame's time went
// (pdmp2.h). esp_timer_get_time() is microseconds and a handful of cycles to
// read; at four reads a frame it is not measurable against a 6 ms encode.
#include "esp_timer.h"
#define PDMP2_TICKS() esp_timer_get_time()
