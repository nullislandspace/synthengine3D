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
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pax_gfx.h"

static char const TAG[] = "se_ppa";

// One client per op type -- the driver ties a client handle to a single
// operation kind. Submission is serialised through a single pump task
// (ppa_pump_task): the game enqueues jobs on s_submit_q; the pump submits
// them to the hardware ONE AT A TIME in order (so execution order ==
// submission order across all op types, no cross-client races) and posts
// each finished job's id to s_done_q for the game to drain via
// se_ppa_wait_job(). Crucially, ppa_do_* is called only from the pump's TASK
// context — never the completion ISR, which only signals.
static ppa_client_handle_t s_fill_client  = NULL;
static ppa_client_handle_t s_srm_client   = NULL;
static ppa_client_handle_t s_blend_client = NULL;
static QueueHandle_t       s_submit_q     = NULL;   // game -> pump: jobs to run
static QueueHandle_t       s_done_q       = NULL;   // pump -> game: finished job ids
static SemaphoreHandle_t   s_op_done_sem  = NULL;   // ISR  -> pump: one op finished
static TaskHandle_t        s_pump_task    = NULL;
static int                 s_inflight     = 0;      // submitted-not-drained; producer task only
static bool                s_inited        = false;

// A queued PPA job: a caller-assigned id plus the fully-built driver config.
// The config is built AND bounds-validated in caller context at submit time
// (so a bad band is refused immediately), then copied by value into the
// queue. The buffer pointers it holds must stay valid until the pump runs it
// — they do; the framebuffer and layer caches outlive a frame.
typedef enum { PPA_JOB_FILL, PPA_JOB_SRM, PPA_JOB_BLEND } ppa_job_type_t;
typedef struct {
    uint32_t       id;
    ppa_job_type_t type;
    union {
        ppa_fill_oper_config_t  fill;
        ppa_srm_oper_config_t   srm;
        ppa_blend_oper_config_t blend;
    } cfg;
} ppa_job_t;

// Completion ISR (shared by all three clients): wake the pump, which is
// blocked taking s_op_done_sem after submitting the in-flight op. ISR-safe
// (give only); the next submit happens back in the pump's task context.
static bool ppa_on_trans_done(ppa_client_handle_t client,
                              ppa_event_data_t* event_data, void* user_data) {
    (void)client;
    (void)event_data;
    (void)user_data;
    BaseType_t hpw = pdFALSE;
    xSemaphoreGiveFromISR(s_op_done_sem, &hpw);
    return hpw == pdTRUE;
}

// Enqueue a built job (caller/producer context). Non-blocking: refuses if the
// submit queue is full. s_inflight is bumped here and decremented only in the
// drain (se_ppa_wait_*) — both run in the single producer task, so it needs
// no locking.
static bool ppa_enqueue(ppa_job_t const* job) {
    if (xQueueSend(s_submit_q, job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "submit queue full (depth %d); job %u refused",
                 (int)SE_PPA_QUEUE_DEPTH, (unsigned)job->id);
        return false;
    }
    s_inflight++;
    return true;
}

// The pump: submit one op, wait for its completion, record its id, repeat.
// Runs in task context, so ppa_do_* (which take blocking driver locks) are
// legal here. One op in flight at a time => strict submission-order execution
// and a per-client driver depth of 1 always suffices.
static void ppa_pump_task(void* arg) {
    (void)arg;
    for (;;) {
        ppa_job_t job;
        if (xQueueReceive(s_submit_q, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        esp_err_t err = ESP_FAIL;
        switch (job.type) {
            case PPA_JOB_FILL:  err = ppa_do_fill(s_fill_client, &job.cfg.fill);              break;
            case PPA_JOB_SRM:   err = ppa_do_scale_rotate_mirror(s_srm_client, &job.cfg.srm); break;
            case PPA_JOB_BLEND: err = ppa_do_blend(s_blend_client, &job.cfg.blend);           break;
        }
        if (err == ESP_OK) {
            if (xSemaphoreTake(s_op_done_sem, pdMS_TO_TICKS(50)) != pdTRUE) {
                ESP_LOGW(TAG, "pump: job %u completion timed out", (unsigned)job.id);
            }
        } else {
            ESP_LOGW(TAG, "pump: job %u (type %d) submit failed: %d",
                     (unsigned)job.id, (int)job.type, err);
        }
        // Always record completion (even on failure) so a waiter never hangs.
        if (xQueueSend(s_done_q, &job.id, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGE(TAG, "pump: done queue full; completion of job %u dropped "
                          "(game not draining?)", (unsigned)job.id);
        }
    }
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

// Generalisation of band_to_raw to an arbitrary logical rectangle
// (log_x, log_y, log_w, log_h) -> raw block. A full-width band is just the
// special case log_x = 0, log_w = full logical width. Same orientation
// rules: under PAX_O_ROT_CW logical (lx,ly) maps to raw (raw_w-1-ly, lx),
// so a logical rect transposes — its logical width becomes raw height and
// its logical height becomes raw width, and the logical-y origin flips into
// raw-x.
static raw_blk_t rect_to_raw(uint32_t raw_w, uint32_t raw_h, pax_orientation_t o,
                             int log_x, int log_y, int log_w, int log_h) {
    raw_blk_t b = {0};
    b.pic_w = raw_w;
    b.pic_h = raw_h;

    switch (o) {
        case PAX_O_UPRIGHT:
            b.block_w        = (uint32_t)log_w;
            b.block_h        = (uint32_t)log_h;
            b.block_offset_x = (uint32_t)log_x;
            b.block_offset_y = (uint32_t)log_y;
            b.ok             = true;
            break;
        case PAX_O_ROT_CW:
            b.block_w        = (uint32_t)log_h;                        // logical-h -> raw-w
            b.block_h        = (uint32_t)log_w;                        // logical-w -> raw-h
            b.block_offset_x = (uint32_t)((int)raw_w - log_y - log_h); // ly flips into raw-x
            b.block_offset_y = (uint32_t)log_x;                        // lx -> raw-y
            b.ok             = true;
            break;
        default:
            b.ok = false;
            return b;
    }

    if (log_w <= 0 || log_h <= 0 || log_x < 0 || log_y < 0 ||
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

    s_submit_q    = xQueueCreate(SE_PPA_QUEUE_DEPTH, sizeof(ppa_job_t));
    s_done_q      = xQueueCreate(SE_PPA_QUEUE_DEPTH, sizeof(uint32_t));
    s_op_done_sem = xSemaphoreCreateBinary();
    if (s_submit_q == NULL || s_done_q == NULL || s_op_done_sem == NULL) {
        ESP_LOGE(TAG, "failed to create pump queues/semaphore");
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

    s_inflight = 0;
    BaseType_t const ok = xTaskCreatePinnedToCore(
        ppa_pump_task, "se_ppa_pump",
        SE_PPA_PUMP_TASK_STACK, NULL,
        SE_PPA_PUMP_TASK_PRIO, &s_pump_task,
        SE_PPA_PUMP_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create se_ppa pump task");
        return false;
    }
    s_inited = true;
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

bool se_ppa_fill(pax_buf_t* fb, uint32_t job_id, int y_top, int h, uint32_t argb) {
    if (!s_inited || fb == NULL) {
        return false;
    }
    raw_blk_t const d = band_to_raw((uint32_t)pax_buf_get_width_raw(fb),
                                    (uint32_t)pax_buf_get_height_raw(fb),
                                    pax_buf_get_orientation(fb), y_top, h);
    if (!d.ok) {
        ESP_LOGW(TAG, "fill: band [%d,+%d) unsupported/out of bounds", y_top, h);
        return false;
    }
    ppa_job_t job = {
        .id   = job_id,
        .type = PPA_JOB_FILL,
        .cfg.fill = {
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
        },
    };
    return ppa_enqueue(&job);
}

bool se_ppa_blit(pax_buf_t* fb, uint32_t job_id, se_ppa_layer_t const* layer, int dst_y_top) {
    if (layer == NULL) {
        return false;
    }
    // Full-width band blit = the sprite blit over the layer's whole extent.
    return se_ppa_blit_rect(fb, job_id, layer, 0, 0,
                            pax_buf_get_width(&layer->buf),
                            pax_buf_get_height(&layer->buf),
                            0, dst_y_top);
}

bool se_ppa_blit_rect(pax_buf_t* fb, uint32_t job_id, se_ppa_layer_t const* layer,
                      int src_x, int src_y, int w, int h,
                      int dst_x, int dst_y) {
    if (!s_inited || fb == NULL || layer == NULL || layer->pixels == NULL) {
        return false;
    }
    // A fully-clipped sprite (nothing to draw) is a success, not an error —
    // callers clip to the screen / horizon and may legitimately pass h <= 0.
    if (w <= 0 || h <= 0) {
        return true;
    }
    pax_buf_t const* lb = &layer->buf;

    // 1:1 copy of a w×h logical sub-rect of the layer (at src_x,src_y) to the
    // framebuffer at (dst_x,dst_y). Both buffers share orientation, so the
    // source and destination raw blocks have identical block_w/block_h
    // (scale = 1) — only the offsets differ.
    raw_blk_t const s = rect_to_raw((uint32_t)pax_buf_get_width_raw(lb),
                                    (uint32_t)pax_buf_get_height_raw(lb),
                                    pax_buf_get_orientation(lb), src_x, src_y, w, h);
    raw_blk_t const d = rect_to_raw((uint32_t)pax_buf_get_width_raw(fb),
                                    (uint32_t)pax_buf_get_height_raw(fb),
                                    pax_buf_get_orientation(fb), dst_x, dst_y, w, h);
    if (!s.ok || !d.ok) {
        ESP_LOGW(TAG, "blit_rect: src(%d,%d %dx%d)/dst(%d,%d) unsupported/out of bounds",
                 src_x, src_y, w, h, dst_x, dst_y);
        return false;
    }
    ppa_job_t job = {
        .id   = job_id,
        .type = PPA_JOB_SRM,
        .cfg.srm = {
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
        },
    };
    return ppa_enqueue(&job);
}

bool se_ppa_blend_key(pax_buf_t* fb, uint32_t job_id, se_ppa_layer_t const* layer,
                      int dst_y_top, uint32_t ck_lo, uint32_t ck_hi) {
    if (!s_inited || fb == NULL || layer == NULL || layer->pixels == NULL) {
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
    ppa_job_t job = {
        .id   = job_id,
        .type = PPA_JOB_BLEND,
        .cfg.blend = {
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
        },
    };
    return ppa_enqueue(&job);
}

void se_ppa_wait_job(uint32_t job_id) {
    // Drain finished-job ids until we pop job_id. Execution is in submission
    // order, so by the time job_id is popped every job submitted before it has
    // also completed and been drained. Each pop decrements the in-flight count.
    while (s_inflight > 0) {
        uint32_t done;
        if (xQueueReceive(s_done_q, &done, pdMS_TO_TICKS(50)) != pdTRUE) {
            ESP_LOGW(TAG, "wait_job(%u) timed out (%d in flight)",
                     (unsigned)job_id, s_inflight);
            return;
        }
        s_inflight--;
        if (done == job_id) {
            return;
        }
    }
}

void se_ppa_wait_all(void) {
    // Drain every submitted-but-not-yet-reaped job.
    while (s_inflight > 0) {
        uint32_t done;
        if (xQueueReceive(s_done_q, &done, pdMS_TO_TICKS(50)) != pdTRUE) {
            ESP_LOGW(TAG, "wait_all timed out (%d in flight)", s_inflight);
            return;
        }
        s_inflight--;
    }
}

int se_ppa_pending(void) {
    return s_inflight;
}
