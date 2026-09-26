// =====================================================================
//  se_stream_audio  --  the mixer's output into the stream (header)
// =====================================================================

#include "se_stream_audio.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "se_audio_source.h"  // AUDIO_SAMPLE_RATE_HZ

// An MPEG-1 audio frame is 1152 samples per channel whatever the layer
// above Layer I, so that is the unit everything here is cut to.
#define MPEG_FRAME_SAMPLES 1152
#define CHANNELS           2

// Enough for about a third of a second, which is far more than the
// stream task can fall behind by without the link being the problem.
#define RING_FRAMES 8

static char const TAG[] = "se_stream_audio";

static int16_t* s_pcm;       // RING_FRAMES * MPEG_FRAME_SAMPLES * CHANNELS
static size_t   s_write;     // samples written by the mixer, modulo the ring
static size_t   s_read;      // samples the encoder has taken
static uint64_t s_samples;   // frames of PCM ever accepted, for the PTS
static bool     s_ready;

// Where the codec goes. It is deliberately a single seam: whatever is
// chosen (D-96) has to turn MPEG_FRAME_SAMPLES x CHANNELS of int16 into
// one MPEG audio frame, and nothing else here changes.
#ifdef SE_STREAM_AUDIO_CODEC
extern bool  se_stream_codec_open(uint32_t rate_hz, int channels);
extern void  se_stream_codec_close(void);
extern size_t se_stream_codec_encode(int16_t const* pcm, uint8_t* out, size_t cap);
#endif

bool se_stream_audio_prepare(void) {
#ifndef SE_STREAM_AUDIO_CODEC
    ESP_LOGW(TAG, "no audio codec compiled in: streaming video only (see se_stream_audio.h)");
    return false;
#else
    size_t const bytes = (size_t)RING_FRAMES * MPEG_FRAME_SAMPLES * CHANNELS * sizeof(int16_t);
    s_pcm              = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);
    if (s_pcm == NULL) return false;
    if (!se_stream_codec_open(AUDIO_SAMPLE_RATE_HZ, CHANNELS)) {
        heap_caps_free(s_pcm);
        s_pcm = NULL;
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
    s_pcm   = NULL;
    s_ready = false;
}

bool se_stream_audio_push(int16_t const* frames, size_t n) {
    if (!s_ready || frames == NULL) return false;
    size_t const cap  = (size_t)RING_FRAMES * MPEG_FRAME_SAMPLES;
    size_t const used = s_write - s_read;
    if (used + n > cap) return false;  // the encoder is behind: drop, do not block

    for (size_t i = 0; i < n; i++) {
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
    if (!s_ready) return false;
    if (s_write - s_read < MPEG_FRAME_SAMPLES) return false;

    static int16_t flat[MPEG_FRAME_SAMPLES * CHANNELS];
    static uint8_t out[2048];
    size_t const   cap = (size_t)RING_FRAMES * MPEG_FRAME_SAMPLES;
    for (size_t i = 0; i < MPEG_FRAME_SAMPLES; i++) {
        size_t const slot      = (s_read + i) % cap;
        flat[i * CHANNELS]     = s_pcm[slot * CHANNELS];
        flat[i * CHANNELS + 1] = s_pcm[slot * CHANNELS + 1];
    }

    // The PTS counts samples, not milliseconds: the stream's own clock,
    // which cannot drift against the audio it describes.
    *pts = 90000 + s_samples * 90000ull / AUDIO_SAMPLE_RATE_HZ;
    s_read += MPEG_FRAME_SAMPLES;
    s_samples += MPEG_FRAME_SAMPLES;

    size_t const n = se_stream_codec_encode(flat, out, sizeof(out));
    if (n == 0) return false;
    *data = out;
    *len  = n;
    return true;
#endif
}
