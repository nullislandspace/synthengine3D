#pragma once
// =====================================================================
//  SynthEngine3D  --  se_ppa.h
//  PUBLIC STABLE API  --  semver'd (see se_version.h)
// ---------------------------------------------------------------------
//  Generic PPA (Pixel-Processing-Accelerator) blit helper for the
//  ESP32-P4. Wraps the driver mechanics every Tanmatsu graceloader app
//  re-writes when it offloads backdrop / sprite work to the PPA:
//
//    * client lifecycle      -- one FILL / SRM / BLEND client, registered
//                               once (the driver ties a client to one op type);
//    * an ordered job queue   -- submits are non-blocking ENQUEUE calls. A
//                               dedicated pump task drains the queue and
//                               submits to the hardware ONE op at a time in
//                               submission order, so execution order ==
//                               submission order ACROSS op types, with no
//                               cross-client races and no caller-managed
//                               waits between ops. (Submission runs in the
//                               pump's task context; the completion ISR only
//                               signals it -- never the unsafe ISR submit.)
//    * the orientation maths  -- logical screen rects -> raw PPA picture
//                               blocks, so callers think in logical
//                               coordinates regardless of display rotation;
//    * PSRAM layer caches      -- cache-line-aligned allocation + the
//                               one-shot C2M flush PPA's DMA needs.
//
//  JOB IDS & COMPLETION: each submit carries a caller-assigned `job_id`
//  (the second argument). The pump posts every finished job's id to a
//  completion queue; se_ppa_wait_job(id) blocks, draining that queue, until
//  `id` is reached -- and because execution is in submission order, when
//  `id` is reached everything submitted before it is done too. IDs are the
//  caller's to scheme; restarting them at 0 each frame is fine as long as
//  the frame drains its jobs (waiting for the last one does that). Typical
//  use: submit a batch with ids 0,1,2..., do CPU work in parallel, then
//  se_ppa_wait_job(last) before touching the result.
//
//  What stays with the CALLER: the artwork drawn into the layers, the band
//  layout (rows / colours / colour-key), and which CPU work overlaps the
//  hardware (enqueue a batch, do CPU work, wait for the id you need).
//
//  THREADING: single producer. All submits + waits must come from one task
//  (the render task). The pump task and the completion ISR are internal.
//
//  Tunables (overridable #defines in se_config.h): SE_PPA_QUEUE_DEPTH (pump
//  submit/done queue depth = max un-drained jobs), SE_PPA_CLIENT_QUEUE_DEPTH
//  (per-client driver depth; 1 suffices under the pump), SE_PPA_PUMP_TASK_*
//  (pump priority / stack / core), SE_PPA_CACHE_LINE (PSRAM line for aligned
//  alloc / msync).
//
//  AVAILABILITY: ESP32-P4 (PPA peripheral) only. Orientation support:
//  PAX_O_UPRIGHT and PAX_O_ROT_CW (the Tanmatsu's panel) are implemented
//  and verified; other orientations are refused (logged) until a port
//  adds and tests their block maths -- a submit just returns false rather
//  than blit to the wrong place.
// =====================================================================

#include "pax_gfx.h"   // pax_buf_t, pax_buf_type_t, pax_orientation_t

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A pre-rendered source layer living in cache-line-aligned PSRAM, wrapped
// in a pax_buf_t so you draw into it with the normal PAX calls. Treat the
// fields as read-only except `buf`, which you draw into. Zero-initialise
// before se_ppa_layer_alloc() (`= {0}`); a zeroed layer is safe to free.
typedef struct {
    pax_buf_t buf;     // draw artwork here, then se_ppa_layer_flush()
    void*     pixels;  // PSRAM backing store (aligned to SE_PPA_CACHE_LINE)
    size_t    size;    // allocation size in bytes (rounded up to a line)
} se_ppa_layer_t;

// Bring up the compositor: register the FILL / SRM / BLEND clients and the
// shared completion semaphore. Idempotent (returns true if already up).
// Returns false (logged) on failure, after which every submit is a no-op
// returning false -- a failed PPA init degrades to "no backdrop", never a
// crash. Call once, from on_init().
bool se_ppa_init(void);

// Allocate a layer cache of `log_w` x `log_h` *logical* pixels in the
// given PAX pixel format / endianness / orientation (match the framebuffer
// you will blit it onto, via se_display_info()). The engine sizes the raw
// backing store for the orientation, aligns it to SE_PPA_CACHE_LINE, and
// pax_buf_init()s `layer->buf` over it. Draw your artwork into `layer->buf`
// with logical coordinates, then call se_ppa_layer_flush() once. Returns
// false (logged) on allocation failure (layer left zeroed). The PSRAM is
// never freed implicitly -- call se_ppa_layer_free() if you must reclaim it.
bool se_ppa_layer_alloc(se_ppa_layer_t* layer, int log_w, int log_h,
                        pax_buf_type_t format, bool reversed,
                        pax_orientation_t orientation);

// Flush a layer's pixels from CPU cache out to PSRAM (cache-to-memory) so
// the PPA's DMA reads the finished artwork. Call once after drawing; the
// caches are static thereafter, so it is not a per-frame cost. No-op on a
// layer that failed to allocate.
void se_ppa_layer_flush(se_ppa_layer_t* layer);

// Free a layer's PSRAM and tear down its pax_buf_t. Safe on a zeroed or
// failed layer (no-op). Most apps never call this (layers live for the
// whole session).
void se_ppa_layer_free(se_ppa_layer_t* layer);

// --- Per-frame submits ------------------------------------------------
// All are NON-BLOCKING: they build + validate the op, ENQUEUE it for the
// pump, and return. The pump runs queued jobs in submission order, one at a
// time. `job_id` (second arg) is a caller-assigned label you later pass to
// se_ppa_wait_job(). Geometry + orientation are read from the pax_buf args.
// Each returns:
//   true  -- enqueued (in flight).
//   false -- REFUSED, nothing queued: the submit queue is full
//            (SE_PPA_QUEUE_DEPTH un-drained jobs -- wait for some and retry),
//            or the orientation/band is unsupported / out of bounds, or PPA
//            is not initialised. All paths are logged. Do NOT wait_job() on
//            an id whose submit returned false (it will time out).

// FILL the band [y_top, y_top + h) of `fb` with `argb` (0xAARRGGBB; alpha
// is ignored for RGB565 targets).
bool se_ppa_fill(pax_buf_t* fb, uint32_t job_id, int y_top, int h, uint32_t argb);

// BLIT (1:1 copy) the whole `layer` into `fb` with the layer's top row at
// logical `dst_y_top` (full layer width at logical x = 0). Convenience
// wrapper over se_ppa_blit_rect for a full-width band layer.
bool se_ppa_blit(pax_buf_t* fb, uint32_t job_id, se_ppa_layer_t const* layer, int dst_y_top);

// BLIT a w×h logical sub-rectangle of `layer` (top-left at src_x,src_y) to
// `fb` at (dst_x,dst_y), 1:1 (no scale/rotate). The sprite form: it lets a
// layer be sized to its artwork's bounding box rather than the full screen
// width, and lets the caller clip a sprite to the screen / a horizon by
// trimming w/h. A non-positive w or h is treated as a fully-clipped (empty)
// sprite and succeeds as a no-op. Both buffers must share orientation (the
// layer alloc matches the fb), so the copy is a plain 1:1 block move.
bool se_ppa_blit_rect(pax_buf_t* fb, uint32_t job_id, se_ppa_layer_t const* layer,
                      int src_x, int src_y, int w, int h,
                      int dst_x, int dst_y);

// BLEND the whole `layer` over `fb` at logical `dst_y_top` with a
// foreground colour-key: a foreground pixel whose PPA-expanded RGB888
// value falls in the inclusive window [ck_lo, ck_hi] (each 0x00RRGGBB) is
// treated as transparent, so `fb` shows through there. Use a tight window
// around a colour that never appears in the artwork. (Note 565->888
// expansion is "shift" on some silicon and "replicate" on others, so widen
// the window across both if keying a near-saturated channel.)
bool se_ppa_blend_key(pax_buf_t* fb, uint32_t job_id, se_ppa_layer_t const* layer,
                      int dst_y_top, uint32_t ck_lo, uint32_t ck_hi);

// --- Completion -------------------------------------------------------

// Block until the job with `job_id` has completed. Drains the completion
// queue up to and including `job_id`; since the pump runs jobs in submission
// order, everything submitted before `job_id` is also done on return.
// Guarded by a 50 ms per-op timeout (logged) so a wedged op can't hang the
// caller forever -- PPA ops are sub-millisecond, so a timeout means something
// is badly wrong.
void se_ppa_wait_job(uint32_t job_id);

// Block until ALL submitted jobs have completed (drain to empty).
void se_ppa_wait_all(void);

// Non-blocking: how many submitted jobs have not yet been drained by a
// se_ppa_wait_* call.
int se_ppa_pending(void);
