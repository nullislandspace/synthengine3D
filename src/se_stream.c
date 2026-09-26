// =====================================================================
//  SynthEngine3D  --  live A/V streaming (se_stream.h)
// ---------------------------------------------------------------------
//  Ported from tanmatsu-nfmtest-grace (main/nfm/stream.c + the switch
//  that drove it). The PPA, encoder and muxer calls are that file's; the
//  frame source, the division of work between tasks and the audio are
//  not.
// =====================================================================

#include "se_stream.h"

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

#define STREAM_PRIO 5  // below usbnet (10), on its core
#define STREAM_CORE 1
#define TASK_STACK  6144
#define SEND_WAIT   2  // ticks a datagram may wait for room in the ring

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

    // Published last, and only now: until this line the stream task has
    // no claim on the buffer.
    int const filled = s_fill;
    s_fill           = (s_fill + 1) % NYUV;
    __sync_synchronize();
    s_ready = filled;
}

// --- audio, handed over by the mixer task (se_stream_audio.h) -----------

void se_stream_tap(int16_t const* frames, size_t n) {
    if (!s_run || !s_cfg.audio || frames == NULL) return;
    int64_t const t0 = esp_timer_get_time();
    if (!se_stream_audio_push(frames, n)) {
        s_st.audio_dropped++;
        return;
    }
    uint32_t const us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > s_st.aud_us_max) s_st.aud_us_max = us;
}

// --- the stream task: everything expensive ------------------------------

static void stream_task(void* arg) {
    (void)arg;
    while (s_run) {
        // Audio first and always: its frames are small and its clock is
        // the one a player trusts. A picture that arrives late is a
        // glitch; sound that arrives late is a gap.
        if (s_cfg.audio) {
            uint8_t const* ab = NULL;
            size_t         an = 0;
            uint64_t       apts = 0;
            while (se_stream_audio_take(&ab, &an, &apts)) {
                tsmux_write_audio(&s_mux, ab, an, apts);
                s_st.audio_frames++;
            }
        }

        int const b = s_ready;
        if (b < 0) {
            vTaskDelay(1);
            continue;
        }

        // The PTS is derived from the frames actually sent and the rate
        // the encoder was configured for. The game's rate varies; the
        // player only needs a clock that advances evenly.
        uint64_t const pts = 90000 + (uint64_t)s_seq * 90000 / (uint64_t)s_cfg.fps_hint;

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
    }
    s_done = true;
    vTaskDelete(NULL);
}

// --- set up and tear down -----------------------------------------------

static void free_all(void) {
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
    // NO AUDIO IS NOT A FAILURE. The stream is worth having without it,
    // and refusing the whole thing because a codec would not start is
    // how a menu row ends up doing nothing at all for a reason nobody
    // can see from the badge.
    if (s_cfg.audio && !se_stream_audio_prepare()) {
        ESP_LOGW(TAG, "no audio in this stream; video only");
        s_cfg.audio = false;
    }

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
    free_all();
    ESP_LOGI(TAG, "stream off: the console is back");
}

void se_stream_get_stats(se_stream_stats_t* out) {
    if (out) *out = s_st;
}
