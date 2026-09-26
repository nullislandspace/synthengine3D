#ifndef PDMP2_H
#define PDMP2_H
/* =====================================================================
 *  pdmp2  --  an MPEG-1/2 Audio Layer II encoder, in the public domain
 * ---------------------------------------------------------------------
 *  SPDX-License-Identifier: CC0-1.0   (see LICENSE)
 *
 *  One frame in, one frame out, no allocation after open, no threads, no
 *  libc beyond memcpy/malloc and libm at open time. Written to be linked
 *  into things that cannot take a copyleft dependency -- which is the
 *  entire reason it exists, because every other MPEG audio encoder
 *  (shine, twolame, lame) is LGPL.
 *
 *      pdmp2_open()            once, with a config
 *      pdmp2_encode_frame()    1152 frames of PCM -> one MPEG frame
 *      pdmp2_close()
 *
 *  WHAT "1152" MEANS. Layer II always codes 1152 samples per channel per
 *  frame, at every sample rate and in both MPEG-1 and MPEG-2 LSF. That
 *  is not true of Layer III, where the LSF frame is 576 -- a difference
 *  that does not fail loudly if you get it wrong, it just never lines up
 *  and the audio silently never starts. Here it is a constant, and
 *  pdmp2_samples_per_frame() returns it so callers need not care.
 *
 *  RATES. MPEG-1 gives 32000/44100/48000; MPEG-2 LSF gives
 *  16000/22050/24000. Both are ordinary Layer II that ffmpeg, OBS, VLC
 *  and every DVB/DAB receiver decode -- LSF is not an exotic mode, it is
 *  how DAB+ and half of digital radio ship. Prefer LSF if your mixer
 *  already runs at 22050: it halves the filterbank work and skips a
 *  resampler, and its bit allocation table is the only one LSF has, so
 *  there is no rate-dependent table selection to get wrong.
 * ===================================================================== */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PDMP2_SAMPLES_PER_FRAME 1152
/* 144 * 384000 / 16000 = 3456, plus a padding byte. Nothing can exceed
 * this, so a caller may keep a fixed buffer if it wants to copy out. */
#define PDMP2_MAX_FRAME_BYTES   3457

typedef struct pdmp2_enc pdmp2_enc_t;

typedef struct {
    int samplerate;    /* 16000 22050 24000 32000 44100 48000 */
    int channels;      /* 1 or 2 */
    int bitrate_kbps;  /* TOTAL, not per channel */
} pdmp2_config_t;

/* 0 if this combination is legal Layer II, negative if not. Call it
 * before open() if you want the reason separated from the allocation
 * failure; open() checks again anyway. */
int pdmp2_check_config(pdmp2_config_t const* cfg);

/* The legal total bitrates for a rate/channel pair, ascending, NULL
 * terminated count in *n. Handy for a menu, and for failing early. */
int const* pdmp2_bitrates(int samplerate, int channels, int* n);

pdmp2_enc_t* pdmp2_open(pdmp2_config_t const* cfg);
void         pdmp2_close(pdmp2_enc_t* e);

/* Always PDMP2_SAMPLES_PER_FRAME. Present so callers can ask instead of
 * assuming, and so the assumption is in one place if that ever changes. */
int pdmp2_samples_per_frame(pdmp2_enc_t const* e);

/* Encode exactly pdmp2_samples_per_frame() frames of INTERLEAVED signed
 * 16-bit PCM (mono: just consecutive samples).
 *
 * Returns a pointer into the encoder's own buffer, valid until the next
 * call to pdmp2_encode_frame() or pdmp2_close(), and writes the length
 * in bytes to *len. Never returns NULL for a valid encoder: a Layer II
 * frame is a fixed size and there is no bit reservoir to run dry, so
 * every call produces exactly one frame.
 *
 * The first frame or two carry the filterbank's startup transient, as
 * they do in every MPEG encoder; feed it silence first if that matters. */
uint8_t const* pdmp2_encode_frame(pdmp2_enc_t* e, int16_t const* pcm, size_t* len);

/* Floats in [-1,1], same contract otherwise. Values outside the range
 * are clipped, not wrapped. */
uint8_t const* pdmp2_encode_frame_f32(pdmp2_enc_t* e, float const* pcm, size_t* len);

#ifdef __cplusplus
}
#endif
#endif /* PDMP2_H */
