// =====================================================================
//  SynthEngine3D  --  se_ppa.c   (see include/se_ppa.h)
//  Generic ESP32-P4 PPA blit helper: client lifecycle, async completion
//  latch, logical-band -> raw-block orientation maths, PSRAM layer caches.
// =====================================================================

#include "se_ppa.h"

#include "se_config.h"

#include <stdint.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pax_gfx.h"

static char const TAG[] = "se_ppa";

// One client per op type -- the driver ties a client handle to a single
// operation kind. All three feed one shared counting semaphore via their
// on_trans_done callbacks; `s_pending_n` counts submitted-but-not-yet-
// reaped ops and is touched only in producer-task context.
static ppa_client_handle_t s_fill_client  = NULL;
static ppa_client_handle_t s_srm_client   = NULL;
static ppa_client_handle_t s_blend_client = NULL;
static SemaphoreHandle_t   s_done_sem      = NULL;
static int                 s_pending_n     = 0;
static bool                s_inited        = false;

// Completion ISR (shared by all three clients): release the latch once per
// finished op. Returns whether a higher-priority task was woken.
static bool ppa_on_trans_done(ppa_client_handle_t client,
                              ppa_event_data_t* event_data, void* user_data) {
    (void)client;
    (void)event_data;
    (void)user_data;
    BaseType_t hpw = pdFALSE;
    xSemaphoreGiveFromISR(s_done_sem, &hpw);
    return hpw == pdTRUE;
}

// --- logical band -> raw picture-block --------------------------------
// The PPA works on raw (pre-orientation) memory. A layer cache and the
// framebuffer are both stored already-rotated, in the SAME orientation, so
// selecting the raw sub-rectangle that corresponds to a full-width logical
// row band [log_y_top, log_y_top + log_h) is a pure index transform -- no
// pixel rotation (SRM rotation_angle stays 0). Only the orientations the
// Tanmatsu panel uses are implemented; others set ok=false and the caller
// refuses the submit.
typedef struct {
    uint32_t pic_w, pic_h;            // full raw picture dims
    uint32_t block_w, block_h;        // raw block dims
    uint32_t block_offset_x, block_offset_y;
    bool     ok;
} raw_blk_t;

static raw_blk_t band_to_raw(uint32_t raw_w, uint32_t raw_h,
                             pax_orientation_t o, int log_y_top, int log_h) {
    raw_blk_t b = {0};
    b.pic_w = raw_w;
    b.pic_h = raw_h;

    switch (o) {
        case PAX_O_UPRIGHT:
            // logical == raw; full logical width => full raw width.
            b.block_w        = raw_w;
            b.block_h        = (uint32_t)log_h;
            b.block_offset_x = 0;
            b.block_offset_y = (uint32_t)log_y_top;
            b.ok             = true;
            break;
        case PAX_O_ROT_CW:
            // raw is logical transposed: raw_w = logical_h, raw_h =
            // logical_w. PAX maps logical (lx,ly) -> raw (raw_w-1-ly, lx),
            // so a full-width band of height log_h becomes a raw column
            // strip of width log_h spanning the full raw height.
            b.block_w        = (uint32_t)log_h;       // logical-h -> raw-w
            b.block_h        = raw_h;                 // full logical width -> full raw_h
            b.block_offset_x = (uint32_t)((int)raw_w - log_y_top - log_h);
            b.block_offset_y = 0;
            b.ok             = true;
            break;
        default:
            // Unsupported on this panel -- refuse rather than mis-blit.
            b.ok = false;
            return b;
    }

    // Bounds guard: a band taller/lower than the picture would index out
    // of the buffer. Validate with signed maths (offset can compute < 0
    // for an out-of-range ROT_CW band) before trusting the unsigned block.
    if (log_h <= 0 || log_y_top < 0 ||
        (int)b.block_offset_x < 0 || (int)b.block_offset_y < 0 ||
        b.block_offset_x + b.block_w > b.pic_w ||
        b.block_offset_y + b.block_h > b.pic_h) {
        b.ok = false;
    }
    return b;
}

bool se_ppa_init(void) {
    if (s_inited) {
        return true;
    }

    s_done_sem = xSemaphoreCreateCounting(SE_PPA_MAX_PENDING, 0);
    if (s_done_sem == NULL) {
        ESP_LOGE(TAG, "failed to create completion semaphore");
        return false;
    }

    ppa_event_callbacks_t const cbs = {.on_trans_done = ppa_on_trans_done};
    ppa_client_config_t const fill_cfg = {
        .oper_type             = PPA_OPERATION_FILL,
        .max_pending_trans_num = SE_PPA_CLIENT_QUEUE_DEPTH,
        .data_burst_length     = PPA_DATA_BURST_LENGTH_128,
    };
    ppa_client_config_t const srm_cfg = {
        .oper_type             = PPA_OPERATION_SRM,
        .max_pending_trans_num = SE_PPA_CLIENT_QUEUE_DEPTH,
        .data_burst_length     = PPA_DATA_BURST_LENGTH_128,
    };
    ppa_client_config_t const blend_cfg = {
        .oper_type             = PPA_OPERATION_BLEND,
        .max_pending_trans_num = SE_PPA_CLIENT_QUEUE_DEPTH,
        .data_burst_length     = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&fill_cfg,  &s_fill_client)  != ESP_OK ||
        ppa_register_client(&srm_cfg,   &s_srm_client)   != ESP_OK ||
        ppa_register_client(&blend_cfg, &s_blend_client) != ESP_OK) {
        ESP_LOGE(TAG, "ppa_register_client failed");
        return false;
    }
    if (ppa_client_register_event_callbacks(s_fill_client,  &cbs) != ESP_OK ||
        ppa_client_register_event_callbacks(s_srm_client,   &cbs) != ESP_OK ||
        ppa_client_register_event_callbacks(s_blend_client, &cbs) != ESP_OK) {
        ESP_LOGE(TAG, "ppa_client_register_event_callbacks failed");
        return false;
    }

    s_pending_n = 0;
    s_inited    = true;
    return true;
}

bool se_ppa_layer_alloc(se_ppa_layer_t* layer, int log_w, int log_h,
                        pax_buf_type_t format, bool reversed,
                        pax_orientation_t orientation) {
    if (layer == NULL) {
        return false;
    }
    *layer = (se_ppa_layer_t){0};
    if (log_w <= 0 || log_h <= 0) {
        ESP_LOGE(TAG, "layer_alloc: bad dims %dx%d", log_w, log_h);
        return false;
    }

    // Raw backing dims: transposed under a quarter-turn, identical
    // otherwise. pax_buf_init() takes raw dimensions; orientation is
    // applied afterwards so the caller draws in logical coordinates.
    int raw_w = log_w;
    int raw_h = log_h;
    if (orientation == PAX_O_ROT_CW || orientation == PAX_O_ROT_CCW ||
        orientation == PAX_O_ROT_CW_FLIP_H || orientation == PAX_O_ROT_CCW_FLIP_H) {
        raw_w = log_h;
        raw_h = log_w;
    }

    size_t const raw_size = pax_buf_calc_size_dynamic(raw_w, raw_h, format);
    size_t const aligned  = (raw_size + SE_PPA_CACHE_LINE - 1) &
                            ~(size_t)(SE_PPA_CACHE_LINE - 1);

    void* pixels = heap_caps_aligned_alloc(SE_PPA_CACHE_LINE, aligned, MALLOC_CAP_SPIRAM);
    if (pixels == NULL) {
        ESP_LOGE(TAG, "layer_alloc: %u B PSRAM failed", (unsigned)aligned);
        return false;
    }

    if (!pax_buf_init(&layer->buf, pixels, raw_w, raw_h, format)) {
        ESP_LOGE(TAG, "layer_alloc: pax_buf_init failed");
        heap_caps_free(pixels);
        return false;
    }
    pax_buf_reversed(&layer->buf, reversed);
    pax_buf_set_orientation(&layer->buf, orientation);
    layer->pixels = pixels;
    layer->size   = aligned;
    return true;
}

void se_ppa_layer_flush(se_ppa_layer_t* layer) {
    if (layer == NULL || layer->pixels == NULL) {
        return;
    }
    esp_err_t err = esp_cache_msync(layer->pixels, layer->size,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                    ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "layer_flush: esp_cache_msync failed: %d", err);
    }
}

void se_ppa_layer_free(se_ppa_layer_t* layer) {
    if (layer == NULL || layer->pixels == NULL) {
        return;
    }
    pax_buf_destroy(&layer->buf);
    heap_caps_free(layer->pixels);
    *layer = (se_ppa_layer_t){0};
}

bool se_ppa_fill(pax_buf_t* fb, int y_top, int h, uint32_t argb) {
    if (!s_inited || fb == NULL) {
        return false;
    }
    if (s_pending_n >= SE_PPA_MAX_PENDING) {
        ESP_LOGW(TAG, "fill: in-flight cap (%d) reached, refused", SE_PPA_MAX_PENDING);
        return false;
    }
    raw_blk_t const d = band_to_raw((uint32_t)pax_buf_get_width_raw(fb),
                                    (uint32_t)pax_buf_get_height_raw(fb),
                                    pax_buf_get_orientation(fb), y_top, h);
    if (!d.ok) {
        ESP_LOGW(TAG, "fill: band [%d,+%d) unsupported/out of bounds", y_top, h);
        return false;
    }
    ppa_fill_oper_config_t cfg = {
        .out = {
            .buffer         = pax_buf_get_pixels_rw(fb),
            .buffer_size    = pax_buf_get_size(fb),
            .pic_w          = d.pic_w,
            .pic_h          = d.pic_h,
            .block_offset_x = d.block_offset_x,
            .block_offset_y = d.block_offset_y,
            .fill_cm        = PPA_FILL_COLOR_MODE_RGB565,
        },
        .fill_block_w    = d.block_w,
        .fill_block_h    = d.block_h,
        .fill_argb_color = {.a = (argb >> 24) & 0xFF, .r = (argb >> 16) & 0xFF,
                            .g = (argb >> 8) & 0xFF,  .b = argb & 0xFF},
        .mode      = PPA_TRANS_MODE_NON_BLOCKING,
        .user_data = NULL,
    };
    esp_err_t err = ppa_do_fill(s_fill_client, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ppa_do_fill failed: %d", err);
        return false;
    }
    s_pending_n++;
    return true;
}

bool se_ppa_blit(pax_buf_t* fb, se_ppa_layer_t const* layer, int dst_y_top) {
    if (!s_inited || fb == NULL || layer == NULL || layer->pixels == NULL) {
        return false;
    }
    if (s_pending_n >= SE_PPA_MAX_PENDING) {
        ESP_LOGW(TAG, "blit: in-flight cap (%d) reached, refused", SE_PPA_MAX_PENDING);
        return false;
    }
    pax_buf_t const* lb = &layer->buf;
    int const log_h = pax_buf_get_height(lb);   // layer's logical band height

    raw_blk_t const s = band_to_raw((uint32_t)pax_buf_get_width_raw(lb),
                                    (uint32_t)pax_buf_get_height_raw(lb),
                                    pax_buf_get_orientation(lb), 0, log_h);
    raw_blk_t const d = band_to_raw((uint32_t)pax_buf_get_width_raw(fb),
                                    (uint32_t)pax_buf_get_height_raw(fb),
                                    pax_buf_get_orientation(fb), dst_y_top, log_h);
    if (!s.ok || !d.ok) {
        ESP_LOGW(TAG, "blit: band at %d unsupported/out of bounds", dst_y_top);
        return false;
    }
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer         = layer->pixels,
            .pic_w          = s.pic_w,
            .pic_h          = s.pic_h,
            .block_w        = s.block_w,
            .block_h        = s.block_h,
            .block_offset_x = s.block_offset_x,
            .block_offset_y = s.block_offset_y,
            .srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer         = pax_buf_get_pixels_rw(fb),
            .buffer_size    = pax_buf_get_size(fb),
            .pic_w          = d.pic_w,
            .pic_h          = d.pic_h,
            .block_offset_x = d.block_offset_x,
            .block_offset_y = d.block_offset_y,
            .srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle    = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x           = 1.0f,
        .scale_y           = 1.0f,
        .mirror_x          = false,
        .mirror_y          = false,
        .rgb_swap          = false,
        .byte_swap         = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode              = PPA_TRANS_MODE_NON_BLOCKING,
        .user_data         = NULL,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_srm_client, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ppa_do_scale_rotate_mirror failed: %d", err);
        return false;
    }
    s_pending_n++;
    return true;
}

bool se_ppa_blend_key(pax_buf_t* fb, se_ppa_layer_t const* layer,
                      int dst_y_top, uint32_t ck_lo, uint32_t ck_hi) {
    if (!s_inited || fb == NULL || layer == NULL || layer->pixels == NULL) {
        return false;
    }
    if (s_pending_n >= SE_PPA_MAX_PENDING) {
        ESP_LOGW(TAG, "blend: in-flight cap (%d) reached, refused", SE_PPA_MAX_PENDING);
        return false;
    }
    pax_buf_t const* lb = &layer->buf;
    int const log_h = pax_buf_get_height(lb);

    raw_blk_t const fg = band_to_raw((uint32_t)pax_buf_get_width_raw(lb),
                                     (uint32_t)pax_buf_get_height_raw(lb),
                                     pax_buf_get_orientation(lb), 0, log_h);
    raw_blk_t const d  = band_to_raw((uint32_t)pax_buf_get_width_raw(fb),
                                     (uint32_t)pax_buf_get_height_raw(fb),
                                     pax_buf_get_orientation(fb), dst_y_top, log_h);
    if (!fg.ok || !d.ok) {
        ESP_LOGW(TAG, "blend: band at %d unsupported/out of bounds", dst_y_top);
        return false;
    }
    ppa_blend_oper_config_t cfg = {
        .in_bg = {
            .buffer         = pax_buf_get_pixels_rw(fb),
            .pic_w          = d.pic_w,
            .pic_h          = d.pic_h,
            .block_w        = d.block_w,
            .block_h        = d.block_h,
            .block_offset_x = d.block_offset_x,
            .block_offset_y = d.block_offset_y,
            .blend_cm       = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .in_fg = {
            .buffer         = layer->pixels,
            .pic_w          = fg.pic_w,
            .pic_h          = fg.pic_h,
            .block_w        = fg.block_w,
            .block_h        = fg.block_h,
            .block_offset_x = fg.block_offset_x,
            .block_offset_y = fg.block_offset_y,
            .blend_cm       = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer         = pax_buf_get_pixels_rw(fb),
            .buffer_size    = pax_buf_get_size(fb),
            .pic_w          = d.pic_w,
            .pic_h          = d.pic_h,
            .block_offset_x = d.block_offset_x,
            .block_offset_y = d.block_offset_y,
            .blend_cm       = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .bg_rgb_swap          = false,
        .bg_byte_swap         = false,
        .bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .fg_rgb_swap          = false,
        .fg_byte_swap         = false,
        .fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,

        // Foreground colour-key window (inclusive, on the expanded RGB888
        // foreground value). Caller supplies the [lo, hi] band.
        .fg_ck_en             = true,
        .fg_ck_rgb_low_thres  = {.r = (ck_lo >> 16) & 0xFF, .g = (ck_lo >> 8) & 0xFF, .b = ck_lo & 0xFF},
        .fg_ck_rgb_high_thres = {.r = (ck_hi >> 16) & 0xFF, .g = (ck_hi >> 8) & 0xFF, .b = ck_hi & 0xFF},
        .bg_ck_en             = false,
        .ck_rgb_default_val   = {.r = 0, .g = 0, .b = 0},
        .ck_reverse_bg2fg     = false,

        .mode      = PPA_TRANS_MODE_NON_BLOCKING,
        .user_data = NULL,
    };
    esp_err_t err = ppa_do_blend(s_blend_client, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ppa_do_blend failed: %d", err);
        return false;
    }
    s_pending_n++;
    return true;
}

void se_ppa_wait_one(void) {
    if (s_pending_n <= 0) {
        return;
    }
    if (xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "wait_one timed out (%d pending)", s_pending_n);
        return;  // intentionally do NOT decrement -- see se_ppa.h
    }
    s_pending_n--;
}

void se_ppa_wait_all(void) {
    while (s_pending_n > 0) {
        int const before = s_pending_n;
        se_ppa_wait_one();
        if (s_pending_n == before) {
            break;  // timed out; avoid spinning on a wedged op
        }
    }
}

int se_ppa_pending(void) {
    // Reap completions that already signalled, without blocking.
    while (s_pending_n > 0 && xSemaphoreTake(s_done_sem, 0) == pdTRUE) {
        s_pending_n--;
    }
    return s_pending_n;
}
