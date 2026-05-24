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
//                               once, sharing a completion semaphore;
//    * async submission       -- non-blocking ops + a counting-semaphore
//                               completion latch (submit many, wait once);
//    * the orientation maths  -- logical screen bands -> raw PPA picture
//                               blocks, so callers think in logical
//                               coordinates regardless of display rotation;
//    * PSRAM layer caches      -- cache-line-aligned allocation + the
//                               one-shot C2M flush PPA's DMA needs.
//
//  What stays with the CALLER (this is deliberate -- it is application
//  policy, not generic PPA work):
//    * the artwork drawn into the layer caches;
//    * the band layout (which rows, which colours, which colour-key);
//    * the *choreography* -- the order of submits, where waits sit, and
//      what CPU work overlaps the hardware. PPA does NOT guarantee
//      execution order across different client handles, so any two ops
//      that touch overlapping pixels must be serialised by the caller
//      (submit -> se_ppa_wait_one -> submit). Independent regions can be
//      fired as a batch and drained with se_ppa_wait_all().
//
//  THREADING: single producer. All submits + waits must come from one
//  task (the render task); the completion ISR only signals. The in-flight
//  counter is mutated only in task context, so there is no lock.
//
//  Tunables (overridable #defines in se_config.h): SE_PPA_MAX_PENDING
//  (in-flight cap -- sizes the semaphore AND the submit guard, so they
//  can never drift), SE_PPA_CLIENT_QUEUE_DEPTH (per-client queue depth),
//  SE_PPA_CACHE_LINE (PSRAM cache-line size for aligned alloc / msync).
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
// All three are NON-BLOCKING: they kick the hardware and return. Geometry
// + orientation are read from the pax_buf arguments. The unit is a
// full-width logical row band [y_top, y_top + h). Each returns:
//   true  -- the op was accepted and is now in flight (pending++).
//   false -- REFUSED, nothing queued, pending unchanged. Either the
//            in-flight cap (SE_PPA_MAX_PENDING) is reached -- drain with
//            se_ppa_wait_one() and retry -- or the driver rejected the op
//            (e.g. per-client queue full), or the orientation/band is
//            unsupported / out of bounds. All paths are logged.
// The counter advances ONLY on an accepted op, so the completion latch
// never desyncs no matter how a submit fails.

// FILL the band [y_top, y_top + h) of `fb` with `argb` (0xAARRGGBB; alpha
// is ignored for RGB565 targets).
bool se_ppa_fill(pax_buf_t* fb, int y_top, int h, uint32_t argb);

// BLIT (scale/rotate/mirror copy, here 1:1) the whole `layer` into `fb`
// with the layer's top row at logical `dst_y_top`. The band height is the
// layer's logical height.
bool se_ppa_blit(pax_buf_t* fb, se_ppa_layer_t const* layer, int dst_y_top);

// BLEND the whole `layer` over `fb` at logical `dst_y_top` with a
// foreground colour-key: a foreground pixel whose PPA-expanded RGB888
// value falls in the inclusive window [ck_lo, ck_hi] (each 0x00RRGGBB) is
// treated as transparent, so `fb` shows through there. Use a tight window
// around a colour that never appears in the artwork. (Note 565->888
// expansion is "shift" on some silicon and "replicate" on others, so widen
// the window across both if keying a near-saturated channel.)
bool se_ppa_blend_key(pax_buf_t* fb, se_ppa_layer_t const* layer,
                      int dst_y_top, uint32_t ck_lo, uint32_t ck_hi);

// --- Completion -------------------------------------------------------

// Block until ONE pending op completes, then return (pending--). No-op if
// nothing is pending. Guarded by a 50 ms timeout: on timeout it logs and
// returns WITHOUT decrementing (a late completion could still signal, and
// decrementing would over-count) -- so a genuinely wedged op surfaces as
// subsequent submits returning false, visibly, rather than as a corrupted
// latch. PPA ops are sub-millisecond, so a timeout means something is
// badly wrong.
void se_ppa_wait_one(void);

// Block until ALL pending ops complete (drains the latch to zero).
void se_ppa_wait_all(void);

// Non-blocking: reap any already-completed ops and return how many are
// still outstanding. Use it to poll progress and free in-flight capacity
// (so a refused submit can succeed on retry) without blocking.
int se_ppa_pending(void);
