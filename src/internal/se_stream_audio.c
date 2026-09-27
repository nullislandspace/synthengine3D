// =====================================================================
//  se_stream_audio  --  the mixer's output into the stream
// =====================================================================

#include "se_stream_audio.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "se_audio_source.h"  // AUDIO_SAMPLE_RATE_HZ

#define CHANNELS 2

// About 200 ms. This is a LIVE stream: the ring's only job is to absorb
// the gap between the mixer's 11.6 ms chunks and a 52 ms MPEG frame, plus
// whatever hitch the game has between offering frames. Depth beyond that
// is not safety, it is latency -- every sample sitting in here is a sample
// arriving late, and it was 8 frames (over 400 ms) which is most of why
// the sound trailed the picture. Overflow drops, by design: a gap in a
// live stream beats a growing delay.
#define RING_FRAMES 4

static char const TAG[] = "se_stream_audio";

static int      s_frame;    // samples a channel per MPEG frame
static int16_t* s_pcm;      // RING_FRAMES * s_frame * CHANNELS
static int16_t* s_flat;     // one frame, de-ringed for the encoder
static size_t   s_write;    // samples written by the mixer, modulo the ring
static size_t   s_read;     // samples the encoder has taken
static uint64_t s_samples;  // frames of PCM the timeline has passed, for the PTS
static uint64_t s_lost;     // frames the writer overwrote before we read them
static bool     s_ready;

// Timed separately because they fail differently: the copy is memory alone
// and the codec is memory plus arithmetic, so if the placement fixed one
// and not the other, one number says so and a total never could.
static uint64_t s_copy_us_sum, s_cenc_us_sum;
static uint32_t s_cost_n;

void se_stream_audio_cost(uint64_t* copy_us, uint64_t* enc_us, uint32_t* n) {
    if (copy_us) *copy_us = s_copy_us_sum;
    if (enc_us) *enc_us = s_cenc_us_sum;
    if (n) *n = s_cost_n;
}

// The codec: pdmp2, a submodule (pdmp2/PROVENANCE.md). Public domain, which
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
#include "pdmp2.h"
#include "pdmp2_port.h"  // PDMP2_PORT_INTERNAL_RESERVE, and where the hot set lives

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
    s_pcm = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);

    // s_flat is INTERNAL, s_pcm is not, and the difference is not
    // inconsistency (pdmp2_port.h, F-105). The ring is streamed: every
    // sample is written once and read once, so a miss on it costs one miss.
    // s_flat is handed to the filterbank, which reads all 1152 samples of
    // it 36 times over -- it belongs with the encoder's other hot buffers.
    // PSRAM, and measured rather than assumed. I put this in internal SRAM
    // first on the theory that the filterbank reads it 36 times over. It
    // does not: encode_frame() converts it to float into the encoder's own
    // pcm buffer in one sequential pass, and the filterbank reads THAT. The
    // timed copy came out at 0.10-0.17 ms and never moved under load, so
    // the internal RAM it was holding is worth more to the encoder's struct
    // (pdmp2.c) and to the USB link.
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
    s_copy_us_sum = s_cenc_us_sum = 0;
    s_cost_n                      = 0;
    s_ready                       = true;
    return true;
#endif
}

void se_stream_audio_reset(void) {
    // CATCH UP TO THE WRITER; do not zero it. The mixer task is already
    // pushing -- it has been since prepare() -- so this runs concurrently
    // with a writer, and zeroing s_write under it is not safe: a push that
    // is mid-flight goes on to store s_write = (its own stale value + n),
    // after which `s_write - s_read` is a huge unsigned number, every
    // later push fails the capacity test, and the audio never recovers.
    //
    // Assigning the writer's cursor to the reader's discards exactly what
    // is queued, touches only the reader's own variable, and is a single
    // word copy. If a push lands during it, at most one 11.6 ms mixer
    // chunk stays behind, which is not worth a lock.
    s_read    = s_write;
    s_samples = 0;
    s_lost    = 0;
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

// OVERWRITES rather than refuses. The old code rejected a chunk when the
// ring was full, which looks like the conservative choice and is not: the
// consumer's sample count is what stamps the audio's PTS, so every refused
// chunk silently SHORTENED THE AUDIO TIMELINE against real time. The error
// does not settle, it accumulates -- ffplay measured A-V at -60 s after
// 130 s of stream, the audio clock running at about half rate.
//
// Overwriting turns that unbounded drift into a bounded glitch: the reader
// notices it was lapped, skips what it cannot have, and advances the
// timeline by the lost amount so the clock stays true. A gap in live audio
// is a click; a clock that drifts is unwatchable within a minute.
bool se_stream_audio_push(int16_t const* frames, size_t n) {
    size_t cap, i;
    if (!s_ready || frames == NULL) return false;
    cap = (size_t)RING_FRAMES * (size_t)s_frame;
    if (n > cap) n = cap;  // a chunk larger than the ring cannot happen; be safe

    for (i = 0; i < n; i++) {
        size_t const slot = (s_write + i) % cap;
        s_pcm[slot * CHANNELS]     = frames[i * CHANNELS];
        s_pcm[slot * CHANNELS + 1] = frames[i * CHANNELS + 1];
    }
    s_write += n;
    return true;
}

uint64_t se_stream_audio_lost(void) {
    return s_lost;
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

    cap = (size_t)RING_FRAMES * (size_t)s_frame;

    // Were we lapped? Then those samples are gone, and the timeline has to
    // be told -- this is the whole point of the overwriting ring above.
    // s_write is the writer's alone and s_read and s_samples are ours, so
    // this needs no lock.
    {
        size_t const avail = s_write - s_read;
        if (avail > cap) {
            size_t const lost = avail - cap;
            s_read += lost;
            s_samples += (uint64_t)lost;  // THE CLOCK MUST NOT SHRINK
            s_lost += (uint64_t)lost;
        }
    }
    if (s_write - s_read < (size_t)s_frame) return false;
    int64_t const t_copy0 = esp_timer_get_time();
    for (i = 0; i < (size_t)s_frame; i++) {
        size_t const slot        = (s_read + i) % cap;
        s_flat[i * CHANNELS]     = s_pcm[slot * CHANNELS];
        s_flat[i * CHANNELS + 1] = s_pcm[slot * CHANNELS + 1];
    }

    // The PTS counts samples, not milliseconds -- but "a sample count
    // cannot drift" is only true while every sample is accounted for,
    // which is what the lap check above is for. It used to not be, and the
    // clock lost about half a second per second.
    //
    // The copy above can also be torn by the writer if it laps us mid-copy.
    // That is one click, bounded, and not worth a lock on the mixer's
    // real-time path.
    int64_t const t_copy1 = esp_timer_get_time();

    *pts = 90000 + s_samples * 90000ull / AUDIO_SAMPLE_RATE_HZ;
    s_read += (size_t)s_frame;
    s_samples += (uint64_t)s_frame;

    n = se_stream_codec_encode(s_flat, &enc);
    s_copy_us_sum += (uint64_t)(t_copy1 - t_copy0);
    s_cenc_us_sum += (uint64_t)(esp_timer_get_time() - t_copy1);
    s_cost_n++;
    if (n == 0 || enc == NULL) return false;
    *data = enc;
    *len  = n;
    return true;
#endif
}

// Where the codec's own time goes, straight out of pdmp2 (pdmp2.h). Split
// because `analyse` reads the buffers that were moved into internal SRAM
// while the other three sweep the subband array, which was not -- so which
// group grows under load says whether the remaining cost is the data or the
// code, and only one of those is fixable without moving app.so.
void se_stream_audio_phases(uint64_t* an, uint64_t* scf, uint64_t* alloc, uint64_t* wr,
                            uint32_t* n) {
#ifdef SE_STREAM_AUDIO_CODEC
    pdmp2_profile_t p;
    pdmp2_profile_get(s_enc, &p);
    if (an) *an = p.analyse;
    if (scf) *scf = p.scalefactors;
    if (alloc) *alloc = p.allocate;
    if (wr) *wr = p.write;
    if (n) *n = p.frames;
#else
    if (an) *an = 0;
    if (scf) *scf = 0;
    if (alloc) *alloc = 0;
    if (wr) *wr = 0;
    if (n) *n = 0;
#endif
}
