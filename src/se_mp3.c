// =====================================================================
//  SynthEngine3D  --  MP3 music source
// ---------------------------------------------------------------------
//  Public API + rationale: include/se_mp3.h.
//
//  The whole design follows from one line in se_audio_source.h: render()
//  runs on the mixer task and "must not block (no FreeRTOS waits, no NVS,
//  no file I/O)". An MP3 needs all three. So the work is split across a
//  lock-free SPSC ring buffer:
//
//      decoder task            ring buffer            mixer task
//      read -> minimp3 ->  resample -> [ ... ]  ->  render() drains
//      (blocks freely)                             (never blocks)
//
//  The ring uses free-running uint32 counters and a power-of-two frame
//  count, so head/tail need no lock: one producer, one consumer, and
//  unsigned wraparound makes (head - tail) the fill level even across the
//  32-bit rollover.
// =====================================================================

#include "se_mp3.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fastopen.h"       // DMA-friendly stdio for /sd (see se_save.c)
#include "ff.h"             // FatFs -- see playlist_scan() for why, not dirent

// minimp3, vendored (public domain). ONLY_MP3 drops MP1/MP2 tables we
// never decode; NO_SIMD because its SIMD paths are x86/ARM only.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

static char const TAG[] = "se_mp3";

#define MP3_DIR_DEFAULT     "/sd/music"
#define MP3_NAME_MAX        64
#define MP3_MAX_TRACKS      128

// PCM ring: power of two so the index mask is a single AND. 16384 frames
// is ~0.74 s at 22050 Hz (64 KB), enough to ride out an SD read stall.
#define MP3_RING_FRAMES     16384u
#define MP3_RING_MASK       (MP3_RING_FRAMES - 1u)

// File read buffer. Large reads keep the SD card's per-transaction
// overhead off the decode loop.
#define MP3_READ_BUFFER     (16 * 1024)
// Refill once the unread tail drops below a whole worst-case frame, so
// the decoder always has a complete frame available.
#define MP3_REFILL_BELOW    (2 * 1024)

// minimp3 is stack-hungry: the plugin this was derived from measured
// >16 KB, and a crash dump here showed ~21 KB in use. NOTE: ESP-IDF's
// xTaskCreate takes the stack size in BYTES, not in StackType_t words as
// vanilla FreeRTOS does -- passing a word count silently yields a stack a
// quarter of the intended size, which fails as a stack-protection panic
// only once a real file is decoded.
#define MP3_TASK_STACK_BYTES (32 * 1024)
// Below the mixer (configMAX_PRIORITIES - 2) so audio always wins, and on
// the mixer's core so the game's render loop on core 0 stays clear.
#define MP3_TASK_PRIORITY    (configMAX_PRIORITIES - 4)
#define MP3_TASK_CORE        1

// Decoder scratch, heap-allocated together so none of it sits on the task
// stack: mp3dec_t is several KB of MDCT/QMF state and one PCM frame is
// another 4.6 KB. Both are touched per frame, so this lives in internal
// SRAM when it can -- PSRAM would add a cache miss to every frame.
typedef struct {
    mp3dec_t dec;
    int16_t  pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
} mp3_scratch_t;

typedef struct {
    music_source_t base;          // MUST be first: we cast between them

    char  dir[96];
    char (*tracks)[MP3_NAME_MAX];
    int   n_tracks;
    int   cur;                    // index of the playing track
    bool  shuffle;
    bool  loop;
    uint32_t rng;

    int16_t* ring;                // MP3_RING_FRAMES * 2 int16
    volatile uint32_t head;       // producer (decoder task)
    volatile uint32_t tail;       // consumer (mixer render)

    uint8_t* read_buf;
    TaskHandle_t task;
    volatile bool stop;           // teardown requested
    volatile bool skip;           // skip the current track
    volatile bool task_done;

    // Resampler state (src rate -> AUDIO_SAMPLE_RATE_HZ), see rs_feed().
    uint32_t rs_ratio;            // input frames per output frame, Q16
    uint32_t rs_phase;            // position within [prev, cur], Q16
    int16_t  rs_prev_l, rs_prev_r;
} se_mp3_t;

// ---- ring buffer ----------------------------------------------------

static inline uint32_t ring_fill(se_mp3_t const* m) {
    return m->head - m->tail;     // unsigned: correct across wraparound
}

// Producer side. Returns false when full; the caller waits and retries.
static inline bool ring_push(se_mp3_t* m, int16_t l, int16_t r) {
    if (ring_fill(m) >= MP3_RING_FRAMES) return false;
    uint32_t const i = m->head & MP3_RING_MASK;
    m->ring[i * 2 + 0] = l;
    m->ring[i * 2 + 1] = r;
    m->head = m->head + 1;
    return true;
}

static void ring_push_blocking(se_mp3_t* m, int16_t l, int16_t r) {
    while (!ring_push(m, l, r)) {
        if (m->stop || m->skip) return;
        vTaskDelay(pdMS_TO_TICKS(4));   // ring full: let the mixer drain
    }
}

// ---- resampler ------------------------------------------------------
//
// Linear interpolation from the file's rate to AUDIO_SAMPLE_RATE_HZ.
// rs_phase is the position of the next output sample inside the current
// input interval [prev, cur], in Q16. Per input frame we emit while the
// phase still lies inside the interval, then shift the window by one
// input frame. This handles up- and down-sampling with the same loop:
// at 44.1 kHz (ratio 2.0) it emits every other frame; at 11.025 kHz
// (ratio 0.5) it emits twice per frame.

#define RS_ONE (1u << 16)

static inline int16_t rs_lerp(int16_t a, int16_t b, uint32_t f) {
    return (int16_t)(a + (int16_t)(((int32_t)(b - a) * (int32_t)f) >> 16));
}

static void rs_reset(se_mp3_t* m, unsigned src_hz) {
    if (src_hz == 0) src_hz = AUDIO_SAMPLE_RATE_HZ;
    m->rs_ratio  = (uint32_t)(((uint64_t)src_hz << 16) / AUDIO_SAMPLE_RATE_HZ);
    if (m->rs_ratio == 0) m->rs_ratio = 1;
    m->rs_phase  = 0;
    m->rs_prev_l = 0;
    m->rs_prev_r = 0;
}

static void rs_feed(se_mp3_t* m, int16_t l, int16_t r) {
    while (m->rs_phase < RS_ONE) {
        ring_push_blocking(m,
                           rs_lerp(m->rs_prev_l, l, m->rs_phase),
                           rs_lerp(m->rs_prev_r, r, m->rs_phase));
        m->rs_phase += m->rs_ratio;
        if (m->stop || m->skip) return;
    }
    m->rs_phase -= RS_ONE;
    m->rs_prev_l = l;
    m->rs_prev_r = r;
}

// ---- playlist -------------------------------------------------------

// Case-insensitive compare. graceloader's ABI exports strcmp/strncmp and
// tolower, but not strcasecmp, so this is spelled out rather than linked.
static int ci_cmp(char const* a, char const* b) {
    for (;;) {
        int const ca = tolower((unsigned char)*a++);
        int const cb = tolower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
        if (ca == 0)  return 0;
    }
}

static bool has_mp3_ext(char const* name) {
    size_t const n = strlen(name);
    if (n < 5) return false;
    char const* e = name + n - 4;
    return (e[0] == '.') &&
           (e[1] == 'm' || e[1] == 'M') &&
           (e[2] == 'p' || e[2] == 'P') &&
           (e[3] == '3');
}

static int name_cmp(void const* a, void const* b) {
    return ci_cmp((char const*)a, (char const*)b);
}

// Open a directory through FatFs.
//
// Why not opendir(): graceloader's ABI exports FatFs (f_opendir / f_readdir /
// f_closedir) but NOT the POSIX dirent wrappers. An app that calls opendir
// links fine and then fails to LOAD, because the symbol cannot be resolved --
// so this has to go through FatFs even though the rest of the file happily
// uses stdio (fopen/fread ARE exported).
//
// FatFs paths are volume-relative and carry no VFS mount point, so the
// "/sd/music" a caller passes is not a FatFs path. Rather than hard-code a
// mapping that would silently break if the mount changed, try the plausible
// spellings and keep whichever opens.
static bool dir_open(FF_DIR* dp, char const* vfs_dir, char* used, size_t used_sz) {
    char const* rel = vfs_dir;
    if      (strncmp(vfs_dir, "/sd",  3) == 0) rel = vfs_dir + 3;
    else if (strncmp(vfs_dir, "/int", 4) == 0) rel = vfs_dir + 4;
    if (*rel == '\0') rel = "/";

    char cand[128];
    for (int i = 0; i < 4; i++) {
        switch (i) {
            case 0:  snprintf(cand, sizeof cand, "%s",   rel);     break;
            case 1:  snprintf(cand, sizeof cand, "0:%s", rel);     break;
            case 2:  snprintf(cand, sizeof cand, "1:%s", rel);     break;
            default: snprintf(cand, sizeof cand, "%s",   vfs_dir); break;
        }
        if (f_opendir(dp, cand) == FR_OK) {
            snprintf(used, used_sz, "%s", cand);
            return true;
        }
    }
    return false;
}

// Scan `dir` for *.mp3. Returns the count (0 = nothing usable).
static int playlist_scan(se_mp3_t* m) {
    FF_DIR d;
    char   opened[128];
    if (!dir_open(&d, m->dir, opened, sizeof opened)) {
        ESP_LOGW(TAG, "cannot open %s (tried FatFs volume-relative forms)", m->dir);
        return 0;
    }
    ESP_LOGI(TAG, "scanning %s (FatFs path %s)", m->dir, opened);

    int      n = 0;
    FILINFO  fno;
    while (n < MP3_MAX_TRACKS && f_readdir(&d, &fno) == FR_OK && fno.fname[0] != 0) {
        if (fno.fattrib & AM_DIR) continue;
        if (!has_mp3_ext(fno.fname)) continue;
        if (strlen(fno.fname) >= MP3_NAME_MAX) continue;
        strcpy(m->tracks[n], fno.fname);
        n++;
    }
    f_closedir(&d);
    // Directory order is whatever the filesystem gives; sort so playback
    // order is predictable and matches what the player sees on a PC.
    if (n > 1) qsort(m->tracks, (size_t)n, MP3_NAME_MAX, name_cmp);
    return n;
}

static void playlist_advance(se_mp3_t* m) {
    if (m->n_tracks <= 1) { m->cur = 0; return; }
    if (m->shuffle) {
        // xorshift32; avoid repeating the track that just played.
        int next = m->cur;
        for (int guard = 0; guard < 8 && next == m->cur; guard++) {
            m->rng ^= m->rng << 13; m->rng ^= m->rng >> 17; m->rng ^= m->rng << 5;
            next = (int)(m->rng % (uint32_t)m->n_tracks);
        }
        m->cur = next;
    } else {
        m->cur = (m->cur + 1) % m->n_tracks;
    }
}

// ---- decoder task ---------------------------------------------------

// Decode one track start to finish, pushing resampled PCM into the ring.
static void decode_track(se_mp3_t* m, mp3_scratch_t* sc) {
    char path[96 + MP3_NAME_MAX + 2];
    snprintf(path, sizeof path, "%s/%s", m->dir, m->tracks[m->cur]);

    FILE* f = fastopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "cannot open %s", path);
        vTaskDelay(pdMS_TO_TICKS(50));   // don't spin on a bad playlist
        return;
    }
    ESP_LOGI(TAG, "playing %s", m->tracks[m->cur]);

    mp3dec_init(&sc->dec);
    rs_reset(m, AUDIO_SAMPLE_RATE_HZ);   // replaced by the first frame's rate

    size_t   fill = 0, pos = 0;
    bool     eof = false, rate_known = false;
    int16_t* const pcm = sc->pcm;

    while (!m->stop && !m->skip) {
        // Top the read buffer up when the unread tail runs low.
        if (!eof && (fill - pos) < MP3_REFILL_BELOW) {
            if (pos > 0) {
                memmove(m->read_buf, m->read_buf + pos, fill - pos);
                fill -= pos;
                pos = 0;
            }
            size_t const space = MP3_READ_BUFFER - fill;
            if (space > 0) {
                size_t const got = fread(m->read_buf + fill, 1, space, f);
                fill += got;
                if (got < space) eof = true;
            }
        }
        if ((fill - pos) == 0) break;   // consumed everything

        mp3dec_frame_info_t info;
        int const frames = mp3dec_decode_frame(&sc->dec, m->read_buf + pos,
                                               (int)(fill - pos), pcm, &info);
        if (info.frame_bytes <= 0) break;         // not resyncable
        pos += (size_t)info.frame_bytes;
        if (frames <= 0) continue;                // header / skipped frame

        if (!rate_known) {
            rs_reset(m, (unsigned)info.hz);
            rate_known = true;
            ESP_LOGI(TAG, "%d Hz, %d ch -> %u Hz stereo",
                     info.hz, info.channels, (unsigned)AUDIO_SAMPLE_RATE_HZ);
        }
        if (info.channels >= 2) {
            for (int i = 0; i < frames; i++) rs_feed(m, pcm[i * 2], pcm[i * 2 + 1]);
        } else {
            for (int i = 0; i < frames; i++) rs_feed(m, pcm[i], pcm[i]);
        }
    }
    fastclose(f);
}

static void mp3_task(void* arg) {
    se_mp3_t* const m = (se_mp3_t*)arg;
    mp3_scratch_t* sc = heap_caps_malloc(sizeof(mp3_scratch_t), MALLOC_CAP_INTERNAL);
    if (sc == NULL) sc = heap_caps_malloc(sizeof(mp3_scratch_t), MALLOC_CAP_SPIRAM);
    if (sc == NULL) {
        ESP_LOGE(TAG, "no memory for the decoder");
        m->task_done = true;
        vTaskDelete(NULL);
        return;
    }

    while (!m->stop) {
        m->skip = false;
        decode_track(m, sc);
        if (m->stop) break;
        bool const was_last = (m->cur == m->n_tracks - 1);
        playlist_advance(m);
        if (!m->loop && was_last && !m->skip) {
            ESP_LOGI(TAG, "playlist finished");
            break;    // go silent; render() emits zeros from here on
        }
    }

    free(sc);
    m->task_done = true;
    vTaskDelete(NULL);
}

// ---- music_source_t -------------------------------------------------

// Mixer side. Bounded copy out of the ring, nothing else -- no locks, no
// I/O, no waits. A shortfall (decoder behind) leaves the rest of the
// pre-zeroed buffer alone, so an underrun is silence, never a stall.
static void mp3_render(music_source_t* self, int16_t* out, size_t frames) {
    se_mp3_t* const m = (se_mp3_t*)self;
    uint32_t const tail = m->tail;
    uint32_t avail = ring_fill(m);
    if (avail > frames) avail = (uint32_t)frames;
    for (uint32_t i = 0; i < avail; i++) {
        uint32_t const idx = (tail + i) & MP3_RING_MASK;
        out[i * 2 + 0] = m->ring[idx * 2 + 0];
        out[i * 2 + 1] = m->ring[idx * 2 + 1];
    }
    m->tail = tail + avail;
}

static void mp3_shutdown(music_source_t* self) {
    se_mp3_t* const m = (se_mp3_t*)self;
    m->stop = true;
    // The task can be parked in a 4 ms ring-full wait; give it room to
    // notice the flag and leave before we free what it is touching.
    for (int i = 0; i < 100 && !m->task_done; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (!m->task_done) ESP_LOGW(TAG, "decoder task did not stop cleanly");
    free(m->ring);
    free(m->read_buf);
    free(m->tracks);
    free(m);
}

music_source_t* se_mp3_create(se_mp3_config_t const* cfg) {
    se_mp3_config_t c = { NULL, false, true };
    if (cfg != NULL) c = *cfg;
    if (c.dir == NULL) c.dir = MP3_DIR_DEFAULT;

    se_mp3_t* m = calloc(1, sizeof(se_mp3_t));
    if (m == NULL) { ESP_LOGE(TAG, "out of memory"); return NULL; }
    snprintf(m->dir, sizeof m->dir, "%s", c.dir);
    m->shuffle = c.shuffle;
    m->loop    = c.loop;
    m->rng     = 0x5EED5EEDu;

    m->tracks = heap_caps_malloc((size_t)MP3_MAX_TRACKS * MP3_NAME_MAX, MALLOC_CAP_SPIRAM);
    if (m->tracks == NULL) { free(m); ESP_LOGE(TAG, "out of memory"); return NULL; }

    m->n_tracks = playlist_scan(m);
    if (m->n_tracks == 0) {
        ESP_LOGW(TAG, "no .mp3 files in %s", m->dir);
        free(m->tracks); free(m);
        return NULL;
    }
    if (m->shuffle) playlist_advance(m);

    m->ring     = heap_caps_malloc(MP3_RING_FRAMES * 2u * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    m->read_buf = heap_caps_malloc(MP3_READ_BUFFER, MALLOC_CAP_SPIRAM);
    if (m->ring == NULL || m->read_buf == NULL) {
        ESP_LOGE(TAG, "buffer allocation failed");
        free(m->ring); free(m->read_buf); free(m->tracks); free(m);
        return NULL;
    }

    m->base.render   = mp3_render;
    m->base.on_seed  = NULL;      // a playlist has nothing to reseed
    m->base.shutdown = mp3_shutdown;

    BaseType_t const ok = xTaskCreatePinnedToCore(
        mp3_task, "se_mp3", MP3_TASK_STACK_BYTES, m,
        MP3_TASK_PRIORITY, &m->task, MP3_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "could not start the decoder task (needs %u KB of stack)",
                 (unsigned)(MP3_TASK_STACK_BYTES / 1024));
        free(m->ring); free(m->read_buf); free(m->tracks); free(m);
        return NULL;
    }

    ESP_LOGI(TAG, "%d track(s) in %s", m->n_tracks, m->dir);
    return &m->base;
}

// ---- small accessors ------------------------------------------------

static se_mp3_t* as_mp3(music_source_t* src) {
    return (src != NULL && src->render == mp3_render) ? (se_mp3_t*)src : NULL;
}

int se_mp3_track_count(music_source_t* src) {
    se_mp3_t* const m = as_mp3(src);
    return (m != NULL) ? m->n_tracks : 0;
}

char const* se_mp3_track_name(music_source_t* src) {
    se_mp3_t* const m = as_mp3(src);
    if (m == NULL || m->n_tracks == 0) return "";
    return m->tracks[m->cur];
}

void se_mp3_skip(music_source_t* src) {
    se_mp3_t* const m = as_mp3(src);
    if (m != NULL) m->skip = true;
}
