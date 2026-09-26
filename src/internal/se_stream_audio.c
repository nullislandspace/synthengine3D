// =====================================================================
//  se_stream_audio  --  the mixer's output into the stream
// =====================================================================

#include "se_stream_audio.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "se_audio_source.h"  // AUDIO_SAMPLE_RATE_HZ

#define CHANNELS 2

// Enough for about half a second, which is far more than the stream task
// can fall behind by without the link being the problem.
#define RING_FRAMES 8

static char const TAG[] = "se_stream_audio";

static int      s_frame;    // samples a channel per MPEG frame
static int16_t* s_pcm;      // RING_FRAMES * s_frame * CHANNELS
static int16_t* s_flat;     // one frame, de-ringed for the encoder
static size_t   s_write;    // samples written by the mixer, modulo the ring
static size_t   s_read;     // samples the encoder has taken
static uint64_t s_samples;  // frames of PCM ever accepted, for the PTS
static bool     s_ready;

// The codec: pdmp2, vendored (pdmp2/PROVENANCE.md). Public domain, which
// is the entire reason it exists -- every other MPEG audio encoder worth
// using is LGPL, and this engine ships as one relinked-by-nobody blob.
//
// MPEG-2 LSF LAYER II AT THE MIXER'S OWN RATE. 22050 Hz is an LSF rate,
// so nothing is resampled between the speaker and the stream, and LSF
// Layer II has exactly one bit allocation table -- no rate-dependent
// selection to get wrong. A Layer II frame is 1152 samples a channel at
// every rate, which is worth stating because Layer III's LSF frame is
// 576 and picking the wrong one does not fail loudly, it simply never
// lines up and the audio quietly never starts.
#ifdef SE_STREAM_AUDIO_CODEC
#include "pdmp2/pdmp2.h"

// Generous: the link is USB and the video beside it is twenty times this.
// 160 is LSF Layer II's ceiling; 128 leaves headroom and is transparent
// enough for anything a game mixer produces.
#define CODEC_BITRATE_KBIT 128

static pdmp2_enc_t* s_enc;

static bool se_stream_codec_open(uint32_t rate_hz, int channels) {
    pdmp2_config_t cfg;
    cfg.samplerate   = (int)rate_hz;
    cfg.channels     = channels;
    cfg.bitrate_kbps = CODEC_BITRATE_KBIT;
    if (pdmp2_check_config(&cfg) != 0) {
        int const* rates;
        int        n;
        ESP_LOGE(TAG, "pdmp2 refuses %u Hz, %d ch, %d kbit/s", (unsigned)rate_hz, channels,
                 CODEC_BITRATE_KBIT);
        rates = pdmp2_bitrates((int)rate_hz, channels, &n);
        if (rates && n > 0) ESP_LOGE(TAG, "legal here: %d..%d kbit/s", rates[0], rates[n - 1]);
        return false;
    }
    s_enc = pdmp2_open(&cfg);
    if (s_enc == NULL) return false;
    s_frame = pdmp2_samples_per_frame(s_enc);
    ESP_LOGI(TAG, "audio: MPEG Layer II, %u Hz, %d ch, %d kbit/s, %d samples a frame",
             (unsigned)rate_hz, channels, CODEC_BITRATE_KBIT, s_frame);
    return true;
}

static void se_stream_codec_close(void) {
    if (s_enc) pdmp2_close(s_enc);
    s_enc = NULL;
}

// pdmp2 returns a pointer into its own buffer, valid until the next call
// -- which is exactly how long the muxer needs it, since the stream task
// writes the frame out before asking for another.
static size_t se_stream_codec_encode(int16_t const* pcm, uint8_t const** out) {
    size_t n = 0;
    *out     = pdmp2_encode_frame(s_enc, pcm, &n);
    return *out ? n : 0;
}
#endif

bool se_stream_audio_prepare(void) {
#ifndef SE_STREAM_AUDIO_CODEC
    ESP_LOGW(TAG, "no audio codec compiled in: streaming video only (see se_stream_audio.h)");
    return false;
#else
    size_t bytes;
    if (!se_stream_codec_open(AUDIO_SAMPLE_RATE_HZ, CHANNELS)) return false;

    // Sized from the encoder, never from a constant: see the note above.
    bytes = (size_t)RING_FRAMES * (size_t)s_frame * CHANNELS * sizeof(int16_t);
    s_pcm  = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);
    s_flat = heap_caps_calloc(1, (size_t)s_frame * CHANNELS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s_pcm == NULL || s_flat == NULL) {
        heap_caps_free(s_pcm);
        heap_caps_free(s_flat);
        s_pcm  = NULL;
        s_flat = NULL;
        se_stream_codec_close();
        return false;
    }
    s_write = s_read = 0;
    s_samples        = 0;
    s_ready          = true;
    return true;
#endif
}

void se_stream_audio_free(void) {
#ifdef SE_STREAM_AUDIO_CODEC
    if (s_ready) se_stream_codec_close();
#endif
    heap_caps_free(s_pcm);
    heap_caps_free(s_flat);
    s_pcm   = NULL;
    s_flat  = NULL;
    s_ready = false;
}

bool se_stream_audio_push(int16_t const* frames, size_t n) {
    size_t cap, used, i;
    if (!s_ready || frames == NULL) return false;
    cap  = (size_t)RING_FRAMES * (size_t)s_frame;
    used = s_write - s_read;
    if (used + n > cap) return false;  // the encoder is behind: drop, do not block

    for (i = 0; i < n; i++) {
        size_t const slot = (s_write + i) % cap;
        s_pcm[slot * CHANNELS]     = frames[i * CHANNELS];
        s_pcm[slot * CHANNELS + 1] = frames[i * CHANNELS + 1];
    }
    s_write += n;
    return true;
}

bool se_stream_audio_take(uint8_t const** data, size_t* len, uint64_t* pts) {
    (void)data;
    (void)len;
    (void)pts;
#ifndef SE_STREAM_AUDIO_CODEC
    return false;
#else
    size_t         cap, i, n;
    uint8_t const* enc = NULL;
    if (!s_ready) return false;
    if (s_write - s_read < (size_t)s_frame) return false;

    cap = (size_t)RING_FRAMES * (size_t)s_frame;
    for (i = 0; i < (size_t)s_frame; i++) {
        size_t const slot        = (s_read + i) % cap;
        s_flat[i * CHANNELS]     = s_pcm[slot * CHANNELS];
        s_flat[i * CHANNELS + 1] = s_pcm[slot * CHANNELS + 1];
    }

    // The PTS counts samples, not milliseconds: the stream's own clock,
    // which cannot drift against the audio it describes.
    *pts = 90000 + s_samples * 90000ull / AUDIO_SAMPLE_RATE_HZ;
    s_read += (size_t)s_frame;
    s_samples += (uint64_t)s_frame;

    n = se_stream_codec_encode(s_flat, &enc);
    if (n == 0 || enc == NULL) return false;
    *data = enc;
    *len  = n;
    return true;
#endif
}
