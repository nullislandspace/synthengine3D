// =====================================================================
//  SynthEngine3D  --  live A/V streaming (se_stream.h)
// ---------------------------------------------------------------------
//  Ported from tanmatsu-nfmtest-grace (main/nfm/stream.c + the switch
//  that drove it). The PPA, encoder and muxer calls are that file's; the
//  frame source, the division of work between tasks and the audio are
//  not.
// =====================================================================

#include "se_stream.h"

#include <stdio.h>
#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_types.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "se_audio.h"
#include "se_stream_audio.h"
#include "se_stream_tap.h"
#include "tsmux.h"
#include "usbnet.h"

#define OUT_W     800
#define OUT_H     480
#define YUV_BYTES (OUT_W * OUT_H * 3 / 2)
#define PORT      5000

// Below usbnet (10), which only moves bytes, and below the mixer, which
// must feed a DMA. ABOVE whatever the game runs on this core: a game task
// that outranks this one does not slow the stream down, it stops the
// stream's CPU altogether -- see the note on WORKER_PRIO in the game's
// chunk_worker.c for what that looked like when measured.
#define STREAM_PRIO 5
#define STREAM_CORE 1
#define TASK_STACK  6144
#define SEND_WAIT   2  // ticks a datagram may wait for room in the ring
// Audio frames one pass of the stream loop may encode before it must let
// the video have a turn.
//
// THIS WAS THREE, AND THREE STARVED THE AUDIO. It was set to guard against
// a livelock if encoding 52 ms of audio took longer than 52 ms -- and then
// measurement said pdmp2 encodes a frame in 71 MICROSECONDS, 700 times
// faster than real time, so that livelock was never possible. What the
// bound did instead was couple the audio's delivery rate to the video
// loop's: at two iterations a second, three frames a pass is six a second
// against the nineteen audio needs, and 36% of the sound went missing.
//
// Eight is ~418 ms of catch-up. At 71 us a frame plus packet building --
// and no datagram of its own any more (tsmux_write_audio) -- a full burst
// costs well under a millisecond, so the bound is a backstop rather than a
// throttle.
#define AUDIO_BURST_MAX 8

// TWO YUV PLANES, not three framebuffers. The game's framebuffer is
// never handed over -- it is read by the PPA while the caller still owns
// it -- so what crosses between tasks is the converted frame, which the
// game never touches. One is being filled, one is being encoded.
#define NYUV 2

static char const TAG[] = "se_stream";

static se_stream_cfg_t       s_cfg;
static se_stream_stats_t     s_st;
static uint8_t*              s_yuv[NYUV];
static uint8_t*              s_bs;
static ppa_client_handle_t   s_ppa;
static esp_h264_enc_handle_t s_enc;
static tsmux_t               s_mux;
static volatile bool         s_run;
static volatile bool         s_done;
static volatile int          s_ready;  // filled and waiting for the encoder, -1 none
static int                   s_fill;
static uint32_t              s_seq;
// WHEN each buffer was captured, and when the stream began. Both in
// esp_timer microseconds, because a PTS has to be REAL TIME -- see the
// note where the video PTS is computed.
static int64_t               s_cap_us[NYUV];
static int64_t               s_t0_us;

/* ------------------------------------------------------------------ *
 *  DIAGNOSTIC HISTORY -- temporary, and here for one reason: while the
 *  stream runs there is no console, no BadgeLink and no log, and the
 *  console comes back too slowly after the stream ends to catch anything
 *  printed at that moment. So the counters are SAMPLED INTO RAM while
 *  streaming and written to a file on the card once it stops.
 *
 *  Nothing touches the card while the stream is running, deliberately: a
 *  FatFs write can block for tens of milliseconds and would be perfectly
 *  capable of causing the stall it is supposed to be measuring.
 * ------------------------------------------------------------------ */
#define HIST_MAX       4096            /* 4096 * 250 ms = 17 minutes */
#define HIST_PERIOD_US 250000
#define HIST_PATH      "/sd/defuckinfo.txt"

typedef struct {
    uint32_t ms;
    uint32_t published, dropped, frames, keyframes;
    uint32_t enc_errors, ppa_errors;
    uint32_t dgrams, dgrams_failed, pcr_only;
    uint32_t audio_frames, audio_dropped;
    uint32_t es_kb, ts_kb;
    uint32_t ppa_us_max, enc_us_max, mux_us_max, aud_us_max;
    // Sums and counts as well as maxima. READING ONLY MAXIMA SENT THIS
    // INVESTIGATION DOWN A BLIND ALLEY TWICE: a single 222 ms frame looks
    // identical to every frame taking 222 ms, and the two call for
    // completely different fixes.
    uint32_t ppa_sum, enc_sum, mux_sum, aud_sum;
    uint32_t ppa_n, enc_n, mux_n, aud_n;
    // CLOSING THE ACCOUNTING. The counters above measured 10 ms of work in
    // a pass that took 1250 ms, which says only that the missing 1240 ms is
    // somewhere they do not look. Three calls in the loop were untimed and
    // all three SEND -- the PCR, each audio frame's mux, and the take that
    // encodes it -- so "blocked in the link" and "never scheduled" were
    // indistinguishable. `pass` is top-of-loop to top-of-loop, so
    // pass - (pcr + atk + amx + enc + mux) is the time the task was not
    // running at all, and the two stop being a matter of opinion.
    uint32_t pass_sum, pcr_sum, atk_sum, amx_sum;
    uint32_t pass_n, pcr_n, atk_n, amx_n;
    uint32_t pass_us_max, pcr_us_max, amx_us_max;
    // The two halves of atk: the ring copy and the codec (pdmp2_port.h).
    uint32_t copy_sum, cenc_sum, cost_n;
    // And the codec's own four phases (pdmp2.h). `an` is the filterbank,
    // which reads only internal SRAM now; the other three sweep the subband
    // array in PSRAM. Which of the two grows under load is the question.
    uint32_t an_sum, scf_sum, alloc_sum, wr_sum, ph_n;
} hist_t;

static uint64_t s_ppa_us_sum, s_enc_us_sum, s_mux_us_sum, s_aud_us_sum;
static uint32_t s_ppa_n, s_enc_n, s_mux_n, s_aud_n;
static uint64_t s_pass_us_sum, s_pcr_us_sum, s_atk_us_sum, s_amx_us_sum;
static uint32_t s_pass_n, s_pcr_n, s_atk_n, s_amx_n;
static uint32_t s_pass_us_max, s_pcr_us_max, s_amx_us_max;

static hist_t* s_hist;
static int     s_hist_n;
static int64_t s_hist_due_us;

static void hist_sample(void) {
    int64_t const now = esp_timer_get_time();
    hist_t*       h;
    if (s_hist == NULL || s_hist_n >= HIST_MAX || now < s_hist_due_us) return;
    s_hist_due_us = now + HIST_PERIOD_US;
    h             = &s_hist[s_hist_n++];
    h->ms            = (uint32_t)((now - s_t0_us) / 1000);
    h->published     = s_st.published;
    h->dropped       = s_st.dropped;
    h->frames        = s_st.frames;
    h->keyframes     = s_st.keyframes;
    h->enc_errors    = s_st.enc_errors;
    h->ppa_errors    = s_st.ppa_errors;
    h->dgrams        = s_st.dgrams;
    h->dgrams_failed = s_st.dgrams_failed;
    h->pcr_only      = s_mux.pcr_only;
    h->audio_frames  = s_st.audio_frames;
    h->audio_dropped = s_st.audio_dropped;
    h->es_kb         = (uint32_t)(s_st.es_bytes / 1024u);
    h->ts_kb         = (uint32_t)(s_st.ts_bytes / 1024u);
    h->ppa_us_max    = s_st.ppa_us_max;
    h->enc_us_max    = s_st.enc_us_max;
    h->mux_us_max    = s_st.mux_us_max;
    h->aud_us_max    = s_st.aud_us_max;
    h->ppa_sum = (uint32_t)s_ppa_us_sum; h->ppa_n = s_ppa_n;
    h->enc_sum = (uint32_t)s_enc_us_sum; h->enc_n = s_enc_n;
    h->mux_sum = (uint32_t)s_mux_us_sum; h->mux_n = s_mux_n;
    h->aud_sum = (uint32_t)s_aud_us_sum; h->aud_n = s_aud_n;
    h->pass_sum = (uint32_t)s_pass_us_sum; h->pass_n = s_pass_n;
    h->pcr_sum  = (uint32_t)s_pcr_us_sum;  h->pcr_n  = s_pcr_n;
    h->atk_sum  = (uint32_t)s_atk_us_sum;  h->atk_n  = s_atk_n;
    h->amx_sum  = (uint32_t)s_amx_us_sum;  h->amx_n  = s_amx_n;
    {
        uint64_t cp = 0, ce = 0;
        uint32_t cn = 0;
        se_stream_audio_cost(&cp, &ce, &cn);
        h->copy_sum = (uint32_t)cp;
        h->cenc_sum = (uint32_t)ce;
        h->cost_n   = cn;
    }
    {
        uint64_t an = 0, scf = 0, al = 0, wr = 0;
        uint32_t pn = 0;
        se_stream_audio_phases(&an, &scf, &al, &wr, &pn);
        h->an_sum    = (uint32_t)an;
        h->scf_sum   = (uint32_t)scf;
        h->alloc_sum = (uint32_t)al;
        h->wr_sum    = (uint32_t)wr;
        h->ph_n      = pn;
    }
    h->pass_us_max = s_pass_us_max;
    h->pcr_us_max  = s_pcr_us_max;
    h->amx_us_max  = s_amx_us_max;
}

/* Called from se_stream_stop(), after the link is down and before the
 * buffers go, which is the first moment the card is safe to touch. */
static void hist_write(void) {
    FILE* f;
    int   i;
    if (s_hist == NULL || s_hist_n == 0) return;
    f = fopen(HIST_PATH, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "could not write " HIST_PATH);
        return;
    }
    fprintf(f, "# SynthEngine3D stream counters, sampled every %d ms\n", HIST_PERIOD_US / 1000);
    fprintf(f, "# cumulative; diff consecutive rows for rates\n");
    fprintf(f, "ms pub drop enc key encerr ppaerr dgram dgfail pcronly aud auddrop eskb tskb "
               "ppamax encmax muxmax audmax ppasum ppan encsum encn muxsum muxn audsum audn "
               "passsum passn pcrsum pcrn atksum atkn amxsum amxn passmax pcrmax amxmax "
               "copysum cencsum costn ansum scfsum allocsum wrsum phn\n");
    for (i = 0; i < s_hist_n; i++) {
        hist_t const* h = &s_hist[i];
        fprintf(f, "%lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu "
                   "%lu %lu %lu %lu %lu %lu %lu %lu "
                   "%lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu "
                   "%lu %lu %lu %lu %lu %lu %lu %lu\n",
                (unsigned long)h->ms, (unsigned long)h->published, (unsigned long)h->dropped,
                (unsigned long)h->frames, (unsigned long)h->keyframes,
                (unsigned long)h->enc_errors, (unsigned long)h->ppa_errors,
                (unsigned long)h->dgrams, (unsigned long)h->dgrams_failed,
                (unsigned long)h->pcr_only, (unsigned long)h->audio_frames,
                (unsigned long)h->audio_dropped, (unsigned long)h->es_kb, (unsigned long)h->ts_kb,
                (unsigned long)h->ppa_us_max, (unsigned long)h->enc_us_max,
                (unsigned long)h->mux_us_max, (unsigned long)h->aud_us_max,
                (unsigned long)h->ppa_sum, (unsigned long)h->ppa_n,
                (unsigned long)h->enc_sum, (unsigned long)h->enc_n,
                (unsigned long)h->mux_sum, (unsigned long)h->mux_n,
                (unsigned long)h->aud_sum, (unsigned long)h->aud_n,
                (unsigned long)h->pass_sum, (unsigned long)h->pass_n,
                (unsigned long)h->pcr_sum, (unsigned long)h->pcr_n,
                (unsigned long)h->atk_sum, (unsigned long)h->atk_n,
                (unsigned long)h->amx_sum, (unsigned long)h->amx_n,
                (unsigned long)h->pass_us_max, (unsigned long)h->pcr_us_max,
                (unsigned long)h->amx_us_max,
                (unsigned long)h->copy_sum, (unsigned long)h->cenc_sum,
                (unsigned long)h->cost_n,
                (unsigned long)h->an_sum, (unsigned long)h->scf_sum,
                (unsigned long)h->alloc_sum, (unsigned long)h->wr_sum,
                (unsigned long)h->ph_n);
    }
    fclose(f);
    ESP_LOGI(TAG, "wrote %d samples to " HIST_PATH, s_hist_n);
}

static bool emit(void* ctx, uint8_t const* d, size_t len) {
    (void)ctx;
    return usbnet_send_udp(PORT, d, (uint16_t)len, SEND_WAIT);
}

bool se_stream_running(void) {
    return s_run;
}

// --- the frame path: runs in the caller, so keep it short ----------------

void se_stream_frame(pax_buf_t* fb) {
    if (!s_run || fb == NULL) return;
    s_st.published++;

    // The encoder still has the last one. Drop this frame rather than
    // hold the game up waiting for it.
    if (s_ready >= 0) {
        s_st.dropped++;
        return;
    }

    void* const pixels = pax_buf_get_pixels_rw(fb);
    if (pixels == NULL) {
        s_st.ppa_errors++;
        return;
    }
    // The game drew with the CPU, so what the PPA is about to read has
    // to be out of the cache first.
    esp_cache_msync(pixels, (size_t)OUT_W * OUT_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    int64_t const               t0  = esp_timer_get_time();
    ppa_srm_oper_config_t const srm = {
        .in =
            {
                .buffer  = pixels,
                .pic_w   = (uint32_t)pax_buf_get_width_raw(fb),
                .pic_h   = (uint32_t)pax_buf_get_height_raw(fb),
                .block_w = (uint32_t)pax_buf_get_width_raw(fb),
                .block_h = (uint32_t)pax_buf_get_height_raw(fb),
                .srm_cm  = PPA_SRM_COLOR_MODE_RGB565,
            },
        .out =
            {
                .buffer      = s_yuv[s_fill],
                .buffer_size = YUV_BYTES,
                .pic_w       = OUT_W,
                .pic_h       = OUT_H,
                .srm_cm      = PPA_SRM_COLOR_MODE_YUV420,
                .yuv_range   = PPA_COLOR_RANGE_LIMIT,
                .yuv_std     = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
            },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,  // undoes the panel's rotation
        .scale_x        = 1.0f,
        .scale_y        = 1.0f,
        .mode           = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(s_ppa, &srm) != ESP_OK) {
        s_st.ppa_errors++;
        return;
    }
    uint32_t const us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > s_st.ppa_us_max) s_st.ppa_us_max = us;
    s_ppa_us_sum += us;
    s_ppa_n++;

    // Published last, and only now: until this line the stream task has
    // no claim on the buffer.
    // Stamped HERE, not in the stream task: this is when the picture
    // existed. The encoder may not get to it for tens of milliseconds.
    s_cap_us[s_fill] = esp_timer_get_time();

    int const filled = s_fill;
    s_fill           = (s_fill + 1) % NYUV;
    __sync_synchronize();
    s_ready = filled;
}

// --- audio, handed over by the mixer task (se_stream_audio.h) -----------

void se_stream_tap(int16_t const* frames, size_t n) {
    if (!s_run || !s_cfg.audio || frames == NULL) return;
    int64_t const t0 = esp_timer_get_time();
    (void)se_stream_audio_push(frames, n);  // never refuses; see the header
    uint32_t const us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > s_st.aud_us_max) s_st.aud_us_max = us;
    s_aud_us_sum += us;
    s_aud_n++;
}

// --- the stream task: everything expensive ------------------------------

static void stream_task(void* arg) {
    (void)arg;
    int64_t last_top_us = 0;
    while (s_run) {
        // Top-of-loop to top-of-loop. Everything else in here is a
        // component of this number, and what it does not account for is
        // time the task spent off the CPU.
        int64_t const top_us = esp_timer_get_time();
        if (last_top_us != 0) {
            uint32_t const pass = (uint32_t)(top_us - last_top_us);
            if (pass > s_pass_us_max) s_pass_us_max = pass;
            s_pass_us_sum += pass;
            s_pass_n++;
        }
        last_top_us = top_us;

        // THE CLOCK FIRST, because it is the thing everything else is
        // scheduled against and it must not inherit the game's frame rate
        // as its cadence (tsmux.c, tsmux_pcr_if_due). Cheap: it sends a
        // packet only when one is actually due -- but "cheap" was an
        // assumption until it was timed, and it flushes, which means it
        // sends, which means it can block.
        tsmux_pcr_if_due(&s_mux, 90000 + (uint64_t)((top_us - s_t0_us) * 9 / 100));
        {
            uint32_t const us = (uint32_t)(esp_timer_get_time() - top_us);
            if (us > s_pcr_us_max) s_pcr_us_max = us;
            s_pcr_us_sum += us;
            s_pcr_n++;
        }
        hist_sample();

        // Audio next: its frames are small and it has a real-time deadline
        // the video does not. A picture that arrives late is a glitch;
        // sound that arrives late is a gap.
        //
        // BOUNDED, AND THAT BOUND IS NOT A TUNING PARAMETER. This loop used
        // to drain until the ring was empty, which is a livelock waiting for
        // a slow encoder: each pass consumes AUDIO_BURST_MAX * 52 ms of
        // audio, and if encoding 52 ms of audio takes longer than 52 ms the
        // mixer refills faster than the loop empties, the loop never exits,
        // and the video below never runs at all. That is not a stutter, it
        // is a picture that lags and then stops -- which is exactly what it
        // did, and it went away entirely with cfg.audio off.
        //
        // With a bound, a too-slow encoder loses audio samples instead of
        // the whole video stream, and the ring's lap accounting keeps the
        // clock honest about it (se_stream_audio.c).
        if (s_cfg.audio) {
            uint8_t const* ab = NULL;
            size_t         an = 0;
            uint64_t       apts = 0;
            int            burst = 0;
            for (;;) {
                int64_t const ta = esp_timer_get_time();
                bool const    got = burst < AUDIO_BURST_MAX
                                 && se_stream_audio_take(&ab, &an, &apts);
                int64_t const tb = esp_timer_get_time();
                s_atk_us_sum += (uint32_t)(tb - ta);
                s_atk_n++;
                if (!got) break;
                tsmux_write_audio(&s_mux, ab, an, apts);
                {
                    uint32_t const us = (uint32_t)(esp_timer_get_time() - tb);
                    if (us > s_amx_us_max) s_amx_us_max = us;
                    s_amx_us_sum += us;
                    s_amx_n++;
                }
                s_st.audio_frames++;
                burst++;
            }
            s_st.audio_dropped = (uint32_t)se_stream_audio_lost();
        }

        int const b = s_ready;
        if (b < 0) {
            vTaskDelay(1);
            continue;
        }

        // THE PTS IS REAL TIME, AND IT HAS TO BE.
        //
        // This used to be `s_seq * 90000 / fps_hint` -- a frame counter
        // scaled by the rate the encoder was configured for -- on the
        // reasoning that a player only needs a clock that advances evenly.
        // That is true of a stream carrying video alone, and wrong as soon
        // as there is a second stream to agree with.
        //
        // fps_hint is a HINT: the game renders at whatever rate it manages
        // and frames offered while the encoder is busy are dropped without
        // advancing s_seq at all. So that clock ran at (actual rate /
        // fps_hint) times real time -- typically well under half. The
        // audio clock, meanwhile, counts SAMPLES, which is real time by
        // construction. Two clocks running at different rates, with the
        // PCR riding this one: the audio's timestamps pull steadily ahead
        // of the stream clock and a player holds them back to match, so
        // the sound arrives late by an amount that GROWS the longer the
        // stream runs. Seconds, within a minute.
        //
        // Real elapsed microseconds, scaled to 90 kHz (90000/1e6 = 9/100).
        uint64_t const pts = 90000 + (uint64_t)((s_cap_us[b] - s_t0_us) * 9 / 100);

        esp_h264_enc_in_frame_t  in   = {.raw_data = {.buffer = s_yuv[b], .len = YUV_BYTES}, .pts = (uint32_t)pts};
        esp_h264_enc_out_frame_t out  = {.raw_data = {.buffer = s_bs, .len = YUV_BYTES}};
        int64_t const            t1   = esp_timer_get_time();
        esp_h264_err_t const     herr = esp_h264_enc_process(s_enc, &in, &out);
        int64_t const            t2   = esp_timer_get_time();

        // Released before the muxing, so the game may fill it again
        // while this frame is still going out over USB.
        __sync_synchronize();
        s_ready = -1;
        s_seq++;

        if (herr != ESP_H264_ERR_OK || out.length == 0) {
            s_st.enc_errors++;
            continue;
        }
        bool const key = out.frame_type == ESP_H264_FRAME_TYPE_IDR || out.frame_type == ESP_H264_FRAME_TYPE_I;
        tsmux_write(&s_mux, s_bs, out.length, pts, key);
        int64_t const t3 = esp_timer_get_time();

        s_st.frames++;
        if (key) s_st.keyframes++;
        s_st.es_bytes += out.length;
        s_st.dgrams        = s_mux.dgrams;
        s_st.dgrams_failed = s_mux.dgrams_failed;
        s_st.ts_bytes      = s_mux.bytes;
        uint32_t const enc = (uint32_t)(t2 - t1), mux = (uint32_t)(t3 - t2);
        if (enc > s_st.enc_us_max) s_st.enc_us_max = enc;
        if (mux > s_st.mux_us_max) s_st.mux_us_max = mux;
        s_enc_us_sum += enc;
        s_enc_n++;
        s_mux_us_sum += mux;
        s_mux_n++;
    }
    s_done = true;
    vTaskDelete(NULL);
}

// --- set up and tear down -----------------------------------------------

static void free_all(void) {
    heap_caps_free(s_hist);
    s_hist = NULL;
    if (s_enc) {
        esp_h264_enc_close(s_enc);
        esp_h264_enc_del(s_enc);
        s_enc = NULL;
    }
    if (s_ppa) {
        ppa_unregister_client(s_ppa);
        s_ppa = NULL;
    }
    for (int i = 0; i < NYUV; i++) {
        heap_caps_free(s_yuv[i]);
        s_yuv[i] = NULL;
    }
    heap_caps_free(s_bs);
    s_bs = NULL;
    se_stream_audio_free();
}

esp_err_t se_stream_start(se_stream_cfg_t const* cfg, pax_buf_t const* fb) {
    if (s_run) return ESP_ERR_INVALID_STATE;
    if (cfg == NULL || fb == NULL) return ESP_ERR_INVALID_ARG;

    s_cfg = *cfg;
    if (s_cfg.fps_hint < 1) s_cfg.fps_hint = 30;
    if (s_cfg.gop < 1) s_cfg.gop = s_cfg.fps_hint;
    if (s_cfg.bitrate_kbit == 0) s_cfg.bitrate_kbit = 3000;
    memset(&s_st, 0, sizeof(s_st));

    int const w = pax_buf_get_width_raw(fb), h = pax_buf_get_height_raw(fb);
    if (pax_buf_get_type(fb) != PAX_BUF_16_565RGB || w * h != OUT_W * OUT_H) return ESP_ERR_NOT_SUPPORTED;

    for (int i = 0; i < NYUV; i++) {
        s_yuv[i] = heap_caps_aligned_calloc(64, 1, YUV_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_yuv[i]) goto nomem;
    }
    s_bs = heap_caps_aligned_calloc(64, 1, YUV_BYTES, MALLOC_CAP_SPIRAM);  // >= input, or the encoder refuses
    if (!s_bs) goto nomem;
    // Diagnostic history; not fatal if it will not fit.
    s_hist        = heap_caps_calloc(HIST_MAX, sizeof(hist_t), MALLOC_CAP_SPIRAM);
    s_hist_n      = 0;
    s_hist_due_us = 0;
    s_ppa_us_sum = s_enc_us_sum = s_mux_us_sum = s_aud_us_sum = 0;
    s_ppa_n = s_enc_n = s_mux_n = s_aud_n = 0;
    s_pass_us_sum = s_pcr_us_sum = s_atk_us_sum = s_amx_us_sum = 0;
    s_pass_n = s_pcr_n = s_atk_n = s_amx_n = 0;
    s_pass_us_max = s_pcr_us_max = s_amx_us_max = 0;
    ppa_client_config_t const pcfg = {
        .oper_type             = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length     = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&pcfg, &s_ppa) != ESP_OK) {
        free_all();
        return ESP_FAIL;
    }
    esp_h264_enc_cfg_hw_t const ecfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop      = (uint8_t)s_cfg.gop,
        .fps      = (uint8_t)s_cfg.fps_hint,
        .res      = {.width = OUT_W, .height = OUT_H},
        .rc       = {.bitrate = s_cfg.bitrate_kbit * 1000u, .qp_min = 16, .qp_max = 40},
    };
    if (esp_h264_enc_hw_new(&ecfg, &s_enc) != ESP_H264_ERR_OK || s_enc == NULL ||
        esp_h264_enc_open(s_enc) != ESP_H264_ERR_OK) {
        free_all();
        return ESP_FAIL;
    }
    // AUDIO LAST OF THE ALLOCATORS, AND AFTER THE ENCODER ON PURPOSE.
    // pdmp2 wants about 16 KB of internal SRAM for its filterbank tables
    // (pdmp2_port.h) and so does the hardware H.264 encoder for its DMA
    // buffers. With this call first, a tight internal heap made
    // esp_h264_enc_open() fail and the whole stream refused to start --
    // the menu row simply would not tick, with the picture sacrificed to
    // the sound (F-106). This way the codec takes what is left and falls
    // back to PSRAM, which costs audio quality of timing, not the stream.
    //
    // NO AUDIO IS NOT A FAILURE. The stream is worth having without it,
    // and refusing the whole thing because a codec would not start is
    // how a menu row ends up doing nothing at all for a reason nobody
    // can see from the badge.
    ESP_LOGI(TAG, "internal heap before audio: %u B free, %u B largest block",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (s_cfg.audio && !se_stream_audio_prepare()) {
        ESP_LOGW(TAG, "no audio in this stream; video only");
        s_cfg.audio = false;
    }
    // What the link is left to work with. Logged because the last round
    // guessed it: the reserve admitted two of the codec's three hot buffers
    // and nothing said so (F-108).
    ESP_LOGI(TAG, "internal heap after audio:  %u B free, %u B largest block",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    tsmux_init(&s_mux, emit, NULL);
    tsmux_set_audio(&s_mux, s_cfg.audio);

    // THE LINK LAST. Up to here every failure could be logged; after it
    // there is no console to log to (se_stream.h), so nothing that can
    // fail in a way worth reading is left.
    ESP_LOGW(TAG, "stream on: the console goes away now, until se_stream_stop()");
    if (usbnet_start(NULL, false) != ESP_OK) {
        free_all();
        ESP_LOGE(TAG, "the USB network would not come up");
        return ESP_FAIL;
    }

    // The mixer must keep feeding, or the audio clock stops whenever the
    // game goes quiet: it powers down and writes nothing at all when
    // nothing is playing (audio_mixer.c). Held awake, it writes silence,
    // and silence is what a stream needs between sounds.
    if (s_cfg.audio) audio_mixer_keep_awake(true);

    s_ready = -1;
    s_fill  = 0;
    s_seq   = 0;
    // The stream's time origin. Every video PTS is measured from here, so
    // it must be set once, when the stream starts -- not per frame.
    s_t0_us = esp_timer_get_time();
    // And the audio starts from the same instant. The mixer has been
    // filling the ring since se_stream_audio_prepare(), all through USB
    // enumeration, so without this the first frame out is several hundred
    // milliseconds old and stamped as current -- a constant delay between
    // sound and picture for the life of the stream.
    if (s_cfg.audio) se_stream_audio_reset();
    s_done  = false;
    s_run   = true;
    xTaskCreatePinnedToCore(stream_task, "se_stream", TASK_STACK, NULL, STREAM_PRIO, NULL, STREAM_CORE);
    return ESP_OK;

nomem:
    free_all();
    return ESP_ERR_NO_MEM;
}

void se_stream_stop(void) {
    if (!s_run) return;
    s_run = false;
    for (int i = 0; !s_done && i < 200; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_cfg.audio) audio_mixer_keep_awake(false);
    usbnet_stop();
    // THE FIRST MOMENT THERE IS ANYWHERE TO PRINT since the stream began,
    // so print what happened. Nothing between start and stop can be
    // logged, which makes "the player saw nothing" a question these
    // counters answer and nothing else does: no `published` means the
    // game never offered a frame, no `frames` means the encoder refused
    // them, no `dgrams` means the muxer emitted nothing, and
    // `dgrams_failed` near `dgrams` means the link took nothing.
    ESP_LOGI(TAG, "stream off: published %lu dropped %lu | frames %lu key %lu enc_err %lu ppa_err %lu",
             (unsigned long)s_st.published, (unsigned long)s_st.dropped, (unsigned long)s_st.frames,
             (unsigned long)s_st.keyframes, (unsigned long)s_st.enc_errors, (unsigned long)s_st.ppa_errors);
    ESP_LOGI(TAG, "stream off: dgrams %lu failed %lu | ts %llu B | audio %lu dropped %lu",
             (unsigned long)s_st.dgrams, (unsigned long)s_st.dgrams_failed,
             (unsigned long long)s_st.ts_bytes, (unsigned long)s_st.audio_frames,
             (unsigned long)s_st.audio_dropped);
    hist_write();
    free_all();
    ESP_LOGI(TAG, "stream off: the console is back");
}

void se_stream_get_stats(se_stream_stats_t* out) {
    if (out) *out = s_st;
}
