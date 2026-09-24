#include "se_scene.h"

#include <math.h>
#include <stdlib.h>         // qsort (depth-order pass)
#include <string.h>

#include "se_config.h"      // DISPLAY_* + RENDER_* projection constants
#include "se_direct565.h"   // direct_565_logical_index, direct_565_pack
#include "esp_cache.h"      // esp_cache_msync (banded renderer)
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"  // esp_ptr_internal (depth-order gather buffers)
#include "esp_timer.h"      // scene_raster_stats per-phase timing
#include "se_light_internal.h"  // se_light_is_on / se_light_shade_tri
#include "se_scene_internal.h"  // scene_textured_reserve (called by se_texture_load)

static char const* TAG = "scene";

// --- Camera -------------------------------------------------------------------
//
// The scene projects through one module-global 6-DOF pinhole camera, set
// once per frame before any geometry is submitted. We cache the rotation
// basis (right / up / forward in world space) on each set, so the per-
// vertex transform is a plain 3x3 multiply and the trig runs once per
// frame, not once per vertex. At zero orientation the basis is exactly
// identity and the world->camera transform reduces to the legacy
// (x - cam.x, y - cam.y, z) subtraction — so render_set_camera(x, y)
// (eye at z = 0, no rotation) projects byte-for-byte like the old fixed
// camera.
static render_camera_t s_camera = { 0.0f, RENDER_CAM_Y, 0.0f, 0.0f, 0.0f, 0.0f };
static float s_right[3] = { 1.0f, 0.0f, 0.0f };
static float s_up[3]    = { 0.0f, 1.0f, 0.0f };
static float s_fwd[3]   = { 0.0f, 0.0f, 1.0f };

// Rebuild the cached world-space basis from yaw / pitch / roll. The
// columns of M = Ry(yaw) * Rx(pitch) * Rz(roll) are the camera's right /
// up / forward axes in world space. At zero angles cosf/sinf return
// exactly 1/0, so the basis is exactly identity (right=+x, up=+y,
// forward=+z) and the projection matches the pre-6DOF engine bit-for-bit.
static void camera_build_basis(float yaw, float pitch, float roll) {
    float const cy = cosf(yaw),   sy = sinf(yaw);
    float const cp = cosf(pitch), sp = sinf(pitch);
    float const cr = cosf(roll),  sr = sinf(roll);
    s_right[0] = cy * cr + sy * sp * sr;
    s_right[1] = cp * sr;
    s_right[2] = -sy * cr + cy * sp * sr;
    s_up[0]    = -cy * sr + sy * sp * cr;
    s_up[1]    = cp * cr;
    s_up[2]    = sy * sr + cy * sp * cr;
    s_fwd[0]   = sy * cp;
    s_fwd[1]   = -sp;
    s_fwd[2]   = cy * cp;
}

void render_set_camera_6dof(float x, float y, float z,
                            float yaw, float pitch, float roll) {
    s_camera.x   = x;   s_camera.y     = y;     s_camera.z    = z;
    s_camera.yaw = yaw; s_camera.pitch = pitch; s_camera.roll = roll;
    camera_build_basis(yaw, pitch, roll);
}

void render_set_camera(float x, float y) {
    // Legacy shorthand: eye on the z = 0 plane, looking straight down +z.
    render_set_camera_6dof(x, y, 0.0f, 0.0f, 0.0f, 0.0f);
}

render_camera_t render_camera(void) {
    return s_camera;
}

// World point -> camera space (right / up / forward components): translate
// by the eye, then rotate by the cached basis. At identity orientation
// this is exactly (x - cam.x, y - cam.y, z - cam.z).
static inline void camera_transform(float x, float y, float z,
                                    float* cx, float* cy, float* cz) {
    float const dx = x - s_camera.x;
    float const dy = y - s_camera.y;
    float const dz = z - s_camera.z;
    *cx = s_right[0] * dx + s_right[1] * dy + s_right[2] * dz;
    *cy = s_up[0]    * dx + s_up[1]    * dy + s_up[2]    * dz;
    *cz = s_fwd[0]   * dx + s_fwd[1]   * dy + s_fwd[2]   * dz;
}

void render_project(float x_w, float y_w, float z_w, float* out_sx, float* out_sy) {
    float cx, cy, cz;
    camera_transform(x_w, y_w, z_w, &cx, &cy, &cz);
    if (cz < 0.01f) cz = 0.01f;  // guard against /0 if a near-clip slips through
    float const inv_z = 1.0f / cz;
    *out_sx = RENDER_HALF_W    + RENDER_FOCAL_LEN * cx * inv_z;
    *out_sy = RENDER_HORIZON_Y - RENDER_FOCAL_LEN * cy * inv_z;
}

// --- Depth encoding -----------------------------------------------------------
//
// Depth is the reciprocal of world-z (1/z), the quantity that
// interpolates linearly in screen space under the pinhole
// projection. Nothing nearer than the near plane is drawn, so 1/z
// peaks at 1/RENDER_NEAR_CLIP_Z; SCENE_DEPTH_SCALE maps that to 64000
// whatever the near plane is -- inside the uint16 range with headroom
// (also for the edge bias below), so the rasterizer never has to clamp
// the high end. Larger encoded value = nearer. At the default near
// plane (0.5) the scale is exactly 32000.
//
// The price of a nearer near plane: the scale shrinks with it, so one
// depth step, z^2 / SCENE_DEPTH_SCALE, grows at every distance, and
// the far limit -- where 1/z encodes to less than 1 and nothing is
// drawn -- moves in to z = SCENE_DEPTH_SCALE.
#define SCENE_DEPTH_SCALE   (64000.0f * RENDER_NEAR_CLIP_Z)

// Wireframe edges are nudged this fraction nearer (in 1/z space)
// before the depth compare, so an edge reliably beats the coplanar
// face it outlines without z-fighting, while still losing to
// genuinely nearer geometry.
#define SCENE_LINE_BIAS     1.02f

// --- Buffers ------------------------------------------------------------------
//
// Depth and the frame-stamp share ONE uint32 cell per pixel:
//   high 16 bits = frame stamp,  low 16 bits = encoded 1/z depth.
// Folding them halves the per-pixel cache-line touches in the rasterizer
// (one combined array + the framebuffer, instead of separate depth and
// stamp planes) — the dominant cost there is PSRAM access latency.
//
// The depth buffer is never cleared. The stamp records the frame number
// that last wrote a cell; a depth counts only when its stamp equals the
// current frame, else it reads as "infinitely far". So every frame starts
// with a logically-empty depth buffer for the cost of one counter
// increment — no full-screen memset — and depth traffic happens only on
// pixels the 3D scene actually draws, not the whole screen.
//
// The stamp is 16-bit, so it wraps every 65536 frames; a pixel covered,
// then left untouched for exactly a 65536-frame multiple, then covered
// again, could mis-resolve for one pixel for one frame. That is invisible
// in practice. Frame 0 is skipped on wrap so an untouched (zero-init) cell
// (stamp 0) never matches a live frame.

#define SCENE_PIXELS  (DISPLAY_LOG_W * DISPLAY_RAW_STRIDE)

// Deferred geometry caps: public and overridable since 2.1, so a game
// can size them to its own frame and a host-side checker can read them
// (se_config.h). Overflow silently drops extra geometry (see scene_tri /
// scene_line). At ~40 B/tri the default triangle buffer is ~160 KB.
#define SCENE_TRI_CAP   SE_SCENE_TRI_CAP
#define SCENE_LINE_CAP  SE_SCENE_LINE_CAP

// The deferred-geometry types are part of the public surface now, so a
// game can write its own renderer against them (se_scene.h). These aliases
// keep the internal spelling unchanged.
typedef se_vtx_t scene_vtx_t;   // { float sx, sy, w } -- w is 1/z, larger = nearer
typedef se_tri_t scene_tri_t;
typedef se_seg_t scene_seg_t;

static uint32_t*    s_ds      = NULL;   // (stamp << 16) | depth, indexed like the fb

// The quarter-resolution depth plane in internal SRAM (se_config.h,
// SE_SCENE_DEPTH16_INTERNAL): plain 16-bit depth, 0 = infinitely far,
// cleared at scene_begin(). s_dz_on says whether THIS frame uses it --
// only frames drawn at scale 2 do; full resolution keeps s_ds.
#define SCENE_DZ_PIXELS ((DISPLAY_LOG_W / 2) * (DISPLAY_RAW_STRIDE / 2))
static uint16_t*    s_dz      = NULL;
static bool         s_dz_on   = false;
static scene_tri_t* s_tris    = NULL;   // accumulated triangles (this frame)
static int          s_tri_n   = 0;
static scene_seg_t* s_lines   = NULL;   // accumulated wireframe edges
static int          s_line_n  = 0;

// Textured triangles: a list of their own, so the flat-triangle list and
// its se_tri_t layout are untouched. NULL until the first texture is
// loaded (scene_textured_reserve), so a game without textures pays
// nothing for it.
static se_ttri_t*   s_ttris       = NULL;
static int          s_ttri_n      = 0;
static bool         s_ttri_failed = false;   // allocation failed: stop retrying

// Points (scene_point): allocated on first use, in PSRAM (se_config.h).
static se_pt_t*     s_pts         = NULL;
static int          s_pt_n        = 0;
static bool         s_pt_failed   = false;

static uint16_t*    s_fb      = NULL;
static bool         s_rev     = false;
static uint16_t     s_frame   = 0;      // current frame tag (never 0 while live)

// Rasterize diagnostics — counts (post-cull) + per-phase wallclock of the
// most recent scene_rasterize(), reported by scene_raster_stats().
static int          s_stat_tri_n   = 0;
static int          s_stat_line_n  = 0;
static int64_t      s_stat_tri_us  = 0;
static int64_t      s_stat_line_us = 0;
static int          s_stat_ttri_n  = 0;
static int64_t      s_stat_ttri_us = 0;

// Pixels COVERED by each pass -- span lengths, summed once per span, not
// once per pixel. A fill-bound renderer's only honest denominator: a
// frame time means nothing without the pixel count that produced it, and
// ns-per-pixel is what says whether the inner loop is bound on
// arithmetic or on the PSRAM the framebuffer and depth plane live in.
static int64_t      s_stat_tri_px  = 0;
static int64_t      s_stat_ttri_px = 0;
// ... and how many SPANS those pixels came in. Pixels alone cannot tell
// a fill loop from its setup: a pass of 200000 pixels in 2000 spans and
// one in 20000 spans cost very different amounts for the same picture,
// and only the second is worth vectorising.
static int64_t      s_stat_tri_sp  = 0;
static int64_t      s_stat_ttri_sp = 0;
// Primitives the lists had no room for.
//
// Overflow DROPS, which is the only sane thing a fixed list can do --
// but it did it silently, and a silent drop is a hole in the world
// that looks like a bug in the game. It has cost this project two
// debugging sessions: once when a view distance quietly lost a third
// of its geometry, and once when half a title screen went missing and
// every other explanation was checked first.
static int          s_stat_tri_drop  = 0;
static int          s_stat_ttri_drop = 0;
static int          s_stat_pt_n    = 0;
static int64_t      s_stat_pt_us   = 0;

// Optional render passes (frustum cull / depth order). Both default OFF
// so scene_render() is behaviour- and byte-identical to the no-op cut
// until a game opts in. See scene_set_options().
static se_scene_options_t s_opts = { false, false };

void scene_init(void) {
    // The combined depth+stamp plane is full-screen (SCENE_PIXELS uint32) —
    // far too big for internal SRAM, and touched only during the rasterize
    // pass, so it stays in PSRAM.
    s_ds = heap_caps_malloc(SCENE_PIXELS * sizeof(uint32_t), MALLOC_CAP_SPIRAM);

#if SE_SCENE_DEPTH16_INTERNAL
    // Before the geometry lists, so it gets the internal SRAM they would
    // have taken; they fall back to PSRAM below (se_config.h). Without
    // room for it, frames at scale 2 use s_ds as they always did.
    s_dz = heap_caps_malloc(SCENE_DZ_PIXELS * sizeof(uint16_t), MALLOC_CAP_INTERNAL);
    if (s_dz != NULL) {
        int64_t const c0 = esp_timer_get_time();
        memset(s_dz, 0, SCENE_DZ_PIXELS * sizeof(uint16_t));
        ESP_LOGI(TAG, "quarter-resolution depth plane: INTERNAL (%uKB, clear %lld us)",
                 (unsigned)(SCENE_DZ_PIXELS * sizeof(uint16_t) / 1024), (long long)(esp_timer_get_time() - c0));
    } else {
        ESP_LOGW(TAG, "no internal SRAM for the quarter-resolution depth plane (%uKB, largest free %uKB): using PSRAM",
                 (unsigned)(SCENE_DZ_PIXELS * sizeof(uint16_t) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    }
#endif

    // The deferred geometry lists go in INTERNAL SRAM when they fit. The
    // emit + cull + order passes run on them while the PPA backdrop DMA
    // saturates the PSRAM bus (see the game's on_backdrop): keeping the lists
    // off PSRAM is what turns that overlap into real parallelism instead of
    // bus contention, and it makes the cull/order compaction cache-fast.
    // Fall back to PSRAM if internal RAM is too tight to hold them.
    size_t const tris_sz  = (size_t)SCENE_TRI_CAP  * sizeof(scene_tri_t);
    size_t const lines_sz = (size_t)SCENE_LINE_CAP * sizeof(scene_seg_t);
    s_tris = heap_caps_malloc(tris_sz, MALLOC_CAP_INTERNAL);
    bool const tris_internal = (s_tris != NULL);
    if (!s_tris)  s_tris = heap_caps_malloc(tris_sz, MALLOC_CAP_SPIRAM);
    s_lines = heap_caps_malloc(lines_sz, MALLOC_CAP_INTERNAL);
    bool const lines_internal = (s_lines != NULL);
    if (!s_lines) s_lines = heap_caps_malloc(lines_sz, MALLOC_CAP_SPIRAM);

    if (!s_ds || !s_tris || !s_lines) {
        ESP_LOGE(TAG, "scene buffer allocation failed (ds=%p tris=%p lines=%p)",
                 s_ds, s_tris, s_lines);
        return;
    }
    ESP_LOGI(TAG, "geometry lists: tris=%s (%uKB) lines=%s (%uKB)",
             tris_internal  ? "INTERNAL" : "PSRAM", (unsigned)(tris_sz  / 1024),
             lines_internal ? "INTERNAL" : "PSRAM", (unsigned)(lines_sz / 1024));
    // One-time clear so no garbage cell's stamp matches the first live frame
    // tag (1). Zeroing the whole cell also zeroes its depth, but depth is
    // only ever read after the stamp says it was written this frame.
    memset(s_ds, 0, SCENE_PIXELS * sizeof(uint32_t));
}

// --- Render scale ---------------------------------------------------------------
//
// Quarter-resolution rendering (scene_set_render_scale): everything up to
// the projection is unchanged -- camera, projection constants, viewport,
// culling and clipping all stay in full-screen coordinates -- and the
// projected screen position is then halved, so the rasterizers sample
// every other pixel and every other line of the full-resolution image
// and write them, without the gaps, into a buffer half the size each
// way. At scale 1 nothing below changes, bit for bit.
static int s_div_next = 1;  // requested; latched at scene_begin()
static int s_div      = 1;  // this frame's
static int s_raw_w      = DISPLAY_RAW_W;       // this frame's target, raw width
static int s_raw_stride = DISPLAY_RAW_STRIDE;  // ... and stride (pixels)

// scene_index() for this frame's target (the full panel, or
// the half-size buffer): same rotated layout, smaller stride and width.
static inline int scene_index(int lx, int ly) {
    return lx * s_raw_stride + (s_raw_w - 1 - ly);
}

static void viewport_apply(void);

// --- Raster target ----------------------------------------------------------
//
// Everything a raster pass writes to, and the columns and rows it may
// touch. The passes below read the target only through s_rt, never the
// frame-level s_fb / s_ds / s_dz / s_vp_*, so one frame can be drawn into
// more than one target: scene_rasterize() points it at the whole frame,
// and the banded renderer at one band of columns in internal SRAM at a
// time (`off` then maps the frame index onto the band buffer).
//
// Kept in one struct so that a second core can later draw other bands at
// the same time: that makes s_rt a per-worker context handed to the
// passes, and the counters below per-worker too, summed at the end.
typedef struct {
    uint16_t* fb;      // colour; pixel i of the frame is fb[i - off]
    uint32_t* ds;      // stamped depth, used when !dz_on
    uint16_t* dz;      // plain 16-bit depth, 0 = infinitely far
    bool      dz_on;
    int       off;
    int       vp_x0, vp_y0, vp_x1, vp_y1;   // inclusive, this frame's target pixels
    int64_t   tri_px, tri_sp, ttri_px, ttri_sp;
} raster_target_t;

static raster_target_t s_rt;

static inline int rt_index(int lx, int ly) {
    return scene_index(lx, ly) - s_rt.off;
}

void scene_set_render_scale(int div) {
    s_div_next = div == 2 ? 2 : 1;
}

int scene_render_scale(void) {
    return s_div_next;
}

void scene_begin(pax_buf_t* fb) {
    s_div        = s_div_next;
    s_raw_w      = DISPLAY_RAW_W / s_div;
    s_raw_stride = DISPLAY_RAW_STRIDE / s_div;
    viewport_apply();
    s_fb     = (uint16_t*)pax_buf_get_pixels(fb);
    s_rev    = fb->reverse_endianness;
    s_tri_n  = 0;
    s_line_n = 0;
    s_ttri_n = 0;
    s_pt_n   = 0;
    s_stat_tri_drop  = 0;
    s_stat_ttri_drop = 0;
    // Advance the frame tag; skip 0 so a zero-initialised stamp cell
    // is never mistaken for "written this frame".
    s_frame++;
    if (s_frame == 0) s_frame = 1;
    // The internal plane has no stamps: it is emptied instead, which
    // for internal SRAM costs less than a stamp compare per pixel.
    s_dz_on = s_dz != NULL && s_div == 2;
    if (s_dz_on) memset(s_dz, 0, SCENE_DZ_PIXELS * sizeof(uint16_t));
}

bool scene_textured_reserve(void) {
    if (s_ttris != NULL) return true;
    if (s_ttri_failed) return false;
    // Internal SRAM first, for the same reason as the flat list (see
    // scene_init): cull + order run on it while the PPA backdrop DMA
    // saturates the PSRAM bus. PSRAM if internal is too tight.
    size_t const sz = (size_t)SE_SCENE_TEXTURED_TRI_CAP * sizeof(se_ttri_t);
    s_ttris = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL);
    bool const internal = (s_ttris != NULL);
    if (s_ttris == NULL) s_ttris = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (s_ttris == NULL) {
        ESP_LOGE(TAG, "textured-triangle list allocation failed (%u bytes)", (unsigned)sz);
        s_ttri_failed = true;
        return false;
    }
    ESP_LOGI(TAG, "textured list: %s (%uKB, %d tris)", internal ? "INTERNAL" : "PSRAM",
             (unsigned)(sz / 1024), SE_SCENE_TEXTURED_TRI_CAP);
    return true;
}

// --- Projection ---------------------------------------------------------------

// Project a *camera-space* point (right, up, forward) to (screen x,
// screen y, 1/z). Callers clip to the near plane first (clip_near), so
// the clamp below is only a guard against a division blowing up. The
// world->camera rotate+translate is done once by camera_transform()
// before this, so a vertex shared between the near cull and the
// projection is transformed only once.
static inline void scene_project_cam(float cx, float cy, float cz, scene_vtx_t* out) {
    if (cz < RENDER_NEAR_CLIP_Z) cz = RENDER_NEAR_CLIP_Z;
    float const inv_z = 1.0f / cz;
    out->sx = RENDER_HALF_W    + RENDER_FOCAL_LEN * cx * inv_z;
    out->sy = RENDER_HORIZON_Y - RENDER_FOCAL_LEN * cy * inv_z;
    out->w  = inv_z;
    if (s_div != 1) {  // quarter resolution: pixel (i, j) samples (2i, 2j)
        out->sx *= 0.5f;
        out->sy *= 0.5f;
    }
}

// --- Viewport -----------------------------------------------------------------
//
// The inclusive pixel rectangle every rasterizer in this file is allowed
// to touch. Defaults to the whole framebuffer, which reproduces the
// pre-viewport engine exactly. Deliberately NOT reset by scene_begin():
// it is a persistent property of how the game frames its 3D view, like
// the projection, not a per-frame one.
//
// The game's rectangle is kept in full-screen pixels (s_uvp_*); the
// rasterizers and the cull pass read s_vp_*, the same rectangle in this
// frame's target pixels (halved at quarter resolution: the target pixels
// whose full-screen sample falls inside it).
static int s_uvp_x0 = 0;
static int s_uvp_y0 = 0;
static int s_uvp_x1 = DISPLAY_LOG_W - 1;
static int s_uvp_y1 = DISPLAY_LOG_H - 1;
static int s_vp_x0  = 0;
static int s_vp_y0  = 0;
static int s_vp_x1  = DISPLAY_LOG_W - 1;
static int s_vp_y1  = DISPLAY_LOG_H - 1;

static void viewport_apply(void) {
    if (s_div == 1) {
        s_vp_x0 = s_uvp_x0, s_vp_y0 = s_uvp_y0, s_vp_x1 = s_uvp_x1, s_vp_y1 = s_uvp_y1;
        return;
    }
    // Target pixel i samples full-screen pixel 2i: inside when
    // x0 <= 2i <= x1. Never empty: a one-pixel rect keeps one pixel.
    s_vp_x0 = (s_uvp_x0 + 1) / 2, s_vp_y0 = (s_uvp_y0 + 1) / 2;
    s_vp_x1 = s_uvp_x1 / 2, s_vp_y1 = s_uvp_y1 / 2;
    if (s_vp_x1 < s_vp_x0) s_vp_x1 = s_vp_x0;
    if (s_vp_y1 < s_vp_y0) s_vp_y1 = s_vp_y0;
}

void scene_set_viewport(se_viewport_t const* vp) {
    if (vp == NULL) {
        s_uvp_x0 = 0;
        s_uvp_y0 = 0;
        s_uvp_x1 = DISPLAY_LOG_W - 1;
        s_uvp_y1 = DISPLAY_LOG_H - 1;
        viewport_apply();
        return;
    }
    int x0 = vp->x, y0 = vp->y;
    int x1 = vp->x + vp->w - 1, y1 = vp->y + vp->h - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > DISPLAY_LOG_W - 1) x1 = DISPLAY_LOG_W - 1;
    if (y1 > DISPLAY_LOG_H - 1) y1 = DISPLAY_LOG_H - 1;
    // An empty or inverted rect would make every bound test fail in ways
    // that are hard to read at a call site. Collapse it to a single pixel
    // instead: the frame goes blank, which is a visible, diagnosable
    // symptom rather than a silent corruption.
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    s_uvp_x0 = x0;
    s_uvp_y0 = y0;
    s_uvp_x1 = x1;
    s_uvp_y1 = y1;
    viewport_apply();
}

se_viewport_t scene_viewport(void) {
    return (se_viewport_t){
        .x = s_uvp_x0, .y = s_uvp_y0, .w = s_uvp_x1 - s_uvp_x0 + 1, .h = s_uvp_y1 - s_uvp_y0 + 1};
}

// --- Rounding, without libm ----------------------------------------------------
//
// ceilf() and floorf() are LIBRARY CALLS, even at -O2: they have to set
// errno, so the compiler cannot fold them into the single RISC-V
// convert instruction the value actually needs. The column scans below
// call them twice per span, and a voxel scene draws tens of thousands
// of spans a frame -- measured at 28000, so 57000 library calls to
// round a number that is already in a float register.
//
// These do it inline. (int) truncates towards zero, which is floor for
// a positive value and ceil for a negative one; the compare corrects
// the other case. Same results as the libm calls for every finite value
// in range, which is all a screen coordinate ever is.
static inline int ceil_i(float v) {
    int const i = (int)v;
    return (v > (float)i) ? i + 1 : i;
}

static inline int floor_i(float v) {
    int const i = (int)v;
    return (v < (float)i) ? i - 1 : i;
}

// --- Triangle rasterizer ------------------------------------------------------

// Fill one vertical run (logical x fixed) with a per-pixel depth
// test. Encoded depth across the run is the affine function
// d(y) = As*x + Bs*y + Cs — already scaled into uint16 units, so the
// inner loop is one float add per pixel (no multiply, no clamp on the
// high end). Under PAX_O_ROT_CW a +1 logical-y step is a -1 step in
// the fb / depth / stamp indices alike.
//
// Every run function exists twice over, one per depth plane: `d16`
// is a compile-time constant at each call (the wrappers below), so each
// copy's inner loop is branch-free on it.
static inline __attribute__((always_inline))
void scene_vrun_body(int lx, int y_top, int y_bot,
                     float As, float Bs, float Cs, uint16_t packed, bool const d16) {
    uint16_t const frame = s_frame;
    uint32_t const fhi   = (uint32_t)frame << 16;   // stamp pre-shifted for the store
    int const idx = rt_index(lx, y_top);
    uint16_t* fp  = s_rt.fb + idx;
    uint32_t* dp  = s_rt.ds + idx;
    uint16_t* zp  = s_rt.dz + idx;
    float     d   = As * (float)lx + Bs * (float)y_top + Cs;
    int       cnt = y_bot - y_top + 1;
    s_rt.tri_px += cnt;
    s_rt.tri_sp++;
    while (cnt-- > 0) {
        int di = (int)d;
        if (di < 0) di = 0;                      // sub-pixel edge overshoot guard
        if (d16) {
            if ((uint16_t)di > *zp) {
                *zp = (uint16_t)di;
                *fp = packed;
            }
            zp--;
        } else {
            uint32_t const cell   = *dp;
            // Stale stamp (≠ this frame) reads as depth 0 (infinitely far).
            uint16_t const stored = ((uint16_t)(cell >> 16) == frame) ? (uint16_t)cell : 0;
            if ((uint16_t)di > stored) {
                *dp = fhi | (uint16_t)di;
                *fp = packed;
            }
            dp--;
        }
        fp--;
        d += Bs;
    }
}

static inline void scene_vrun(int lx, int y_top, int y_bot,
                              float As, float Bs, float Cs, uint16_t packed) {
    if (lx < s_rt.vp_x0 || lx > s_rt.vp_x1) return;
    if (y_top < s_rt.vp_y0) y_top = s_rt.vp_y0;
    if (y_bot > s_rt.vp_y1) y_bot = s_rt.vp_y1;
    if (y_top > y_bot) return;
    if (s_rt.dz_on) scene_vrun_body(lx, y_top, y_bot, As, Bs, Cs, packed, true);
    else         scene_vrun_body(lx, y_top, y_bot, As, Bs, Cs, packed, false);
}

// Depth-tested flat-shaded triangle. Same logical-X column scan as
// direct_565_tri (contiguous raw runs, cache-friendly). The depth
// plane d = As*x + Bs*y + Cs (in encoded uint16 units) is derived
// from the three vertices' 1/z values before the x-sort.
static void scene_raster_tri(scene_vtx_t a, scene_vtx_t b, scene_vtx_t c,
                             uint16_t packed) {
    // Plane through the three (sx, sy, w) points; nz near zero is a
    // degenerate (zero-area) triangle — skip it.
    float const ex1 = b.sx - a.sx, ey1 = b.sy - a.sy, ew1 = b.w - a.w;
    float const ex2 = c.sx - a.sx, ey2 = c.sy - a.sy, ew2 = c.w - a.w;
    float const nx  = ey1 * ew2 - ew1 * ey2;
    float const ny  = ew1 * ex2 - ex1 * ew2;
    float const nz  = ex1 * ey2 - ey1 * ex2;
    if (nz > -1e-6f && nz < 1e-6f) return;
    float const inv_nz = 1.0f / nz;
    // Plane coefficients, pre-scaled into encoded-depth units so the
    // per-pixel run does no multiply.
    float const As = (-nx * inv_nz) * SCENE_DEPTH_SCALE;
    float const Bs = (-ny * inv_nz) * SCENE_DEPTH_SCALE;
    float const Cs = a.w * SCENE_DEPTH_SCALE - As * a.sx - Bs * a.sy;

    // Sort vertices so x0 <= x1 <= x2 (w is now captured in As/Bs/Cs).
    float x0 = a.sx, y0 = a.sy, x1 = b.sx, y1 = b.sy, x2 = c.sx, y2 = c.sy;
    float tx, ty;
    if (x1 < x0) { tx=x0; ty=y0; x0=x1; y0=y1; x1=tx; y1=ty; }
    if (x2 < x0) { tx=x0; ty=y0; x0=x2; y0=y2; x2=tx; y2=ty; }
    if (x2 < x1) { tx=x1; ty=y1; x1=x2; y1=y2; x2=tx; y2=ty; }

    if (x2 < (float)s_rt.vp_x0 || x0 > (float)s_rt.vp_x1) return;
    if (x2 - x0 < 1e-6f) return;

    float const dydx_02 = (y2 - y0) / (x2 - x0);
    float const dydx_01 = (x1 > x0) ? (y1 - y0) / (x1 - x0) : 0.0f;
    float const dydx_12 = (x2 > x1) ? (y2 - y1) / (x2 - x1) : 0.0f;

    int ix_start =     ceil_i(x0);
    int ix_split =     ceil_i(x1);
    int ix_endex = 1 + floor_i(x2);
    if (ix_start < s_rt.vp_x0)      ix_start = s_rt.vp_x0;
    if (ix_endex > s_rt.vp_x1 + 1)  ix_endex = s_rt.vp_x1 + 1;
    if (ix_split < ix_start)      ix_split = ix_start;
    if (ix_split > ix_endex)      ix_split = ix_endex;

    for (int x = ix_start; x < ix_split; x++) {
        float const dx = (float)x - x0;
        float const ya = y0 + dydx_02 * dx;
        float const yb = y0 + dydx_01 * dx;
        float yt, yz;
        if (ya < yb) { yt = ya; yz = yb; } else { yt = yb; yz = ya; }
        scene_vrun(x, ceil_i(yt), floor_i(yz), As, Bs, Cs, packed);
    }
    for (int x = ix_split; x < ix_endex; x++) {
        float const dx02 = (float)x - x0;
        float const dx12 = (float)x - x1;
        float const ya   = y0 + dydx_02 * dx02;
        float const yb   = y1 + dydx_12 * dx12;
        float yt, yz;
        if (ya < yb) { yt = ya; yz = yb; } else { yt = yb; yz = ya; }
        scene_vrun(x, ceil_i(yt), floor_i(yz), As, Bs, Cs, packed);
    }
}

// --- Textured triangle rasterizer ---------------------------------------------
//
// The flat path above is left exactly as it was; this is a sibling, not
// a generalisation of it. Same logical-X column scan, same depth plane
// and depth test, same viewport clip. The difference is what happens to
// a pixel that passes the depth test.
//
// Three quantities are affine in screen space under the pinhole
// projection: w = 1/z, and u*w, v*w. So each gets a plane A*x + B*y + C,
// and a +1 step down a column is one add per plane. The texel is
// (u*w)/w, (v*w)/w: perspective-correct, at the price of one divide per
// DRAWN pixel. The depth test comes first, so a pixel that loses to
// nearer geometry never pays for the divide or the texel fetch.
//
// All three planes carry the same SCENE_DEPTH_SCALE factor, so the
// encoded depth the test already has is also the divisor: no extra
// multiply to get w back.

typedef struct {
    float           Ad, Bd, Cd;   // encoded depth, w * SCENE_DEPTH_SCALE
    float           Au, Bu, Cu;   // u * w * SCENE_DEPTH_SCALE, u in texels
    float           Av, Bv, Cv;   // v * w * SCENE_DEPTH_SCALE, v in texels
    uint16_t const* texels;
    uint32_t        wmask, hmask;
    uint32_t        wlog2;
    uint32_t        shade;        // 0..32, red and green
    uint32_t        shade_b;      // 0..32, blue -- differs only while a tint is set
} ttri_setup_t;

// The scene tint (se_scene.h). 32/32 is "no tint", and then the plain
// raster loops run, byte for byte what they always did.
static uint8_t s_tint_rg = 32, s_tint_b = 32;
static bool    s_tint_on;

void se_scene_set_tint(uint8_t rg, uint8_t b) {
    s_tint_rg = rg > 32 ? 32 : rg;
    s_tint_b  = b > 32 ? 32 : b;
    s_tint_on = s_tint_rg != 32 || s_tint_b != 32;
}

static inline __attribute__((always_inline))
void scene_vrun_tex_body(int lx, int y_top, int y_bot, ttri_setup_t const* st, bool const d16) {
    uint16_t const  frame = s_frame;
    uint32_t const  fhi   = (uint32_t)frame << 16;
    int const       idx   = rt_index(lx, y_top);
    uint16_t*       fp    = s_rt.fb + idx;
    uint32_t*       dp    = s_rt.ds + idx;
    uint16_t*       zp    = s_rt.dz + idx;
    float const     fx    = (float)lx, fy = (float)y_top;
    float           d     = st->Ad * fx + st->Bd * fy + st->Cd;
    float           us    = st->Au * fx + st->Bu * fy + st->Cu;
    float           vs    = st->Av * fx + st->Bv * fy + st->Cv;
    float const     Bd = st->Bd, Bu = st->Bu, Bv = st->Bv;
    uint16_t const* tx    = st->texels;
    uint32_t const  wm = st->wmask, hm = st->hmask, wl = st->wlog2;
    uint32_t const  sh    = st->shade;
    bool const      rev   = s_rev;
    int             cnt   = y_bot - y_top + 1;
    s_rt.ttri_px += cnt;
    s_rt.ttri_sp++;
    while (cnt-- > 0) {
        int di = (int)d;
        if (di < 0) di = 0;
        uint16_t const stored =
            d16 ? *zp : ((uint16_t)(*dp >> 16) == frame) ? (uint16_t)*dp : 0;
        if ((uint16_t)di > stored) {
            if (d16) *zp = (uint16_t)di;
            else     *dp = fhi | (uint16_t)di;
            // di >= 1 here, so d >= 1: the divide is safe.
            float const    inv = 1.0f / d;
            uint32_t const tu  = (uint32_t)(int)(us * inv) & wm;
            uint32_t const tv  = (uint32_t)(int)(vs * inv) & hm;
            uint32_t const t   = tx[(tv << wl) | tu];
            // Scale all three RGB565 channels with one multiply: spread
            // G into the upper half-word, leaving 5-6 bits of headroom
            // above each field, multiply by the 0..32 shade, shift back.
            uint32_t x = (t | (t << 16)) & 0x07E0F81Fu;
            x          = ((x * sh) >> 5) & 0x07E0F81Fu;
            uint16_t px = (uint16_t)(x | (x >> 16));
            if (rev) px = (uint16_t)((px >> 8) | (px << 8));
            *fp = px;
        }
        fp--;
        if (d16) zp--;
        else     dp--;
        d  += Bd;
        us += Bu;
        vs += Bv;
    }
}

// The same run, TINTED: red and green scaled by one factor and blue by
// another (se_scene.h, se_scene_set_tint). A sibling rather than a
// branch in the loop above, exactly as the cut-out body below is, so an
// untinted scene runs the code it always ran.
//
// One multiply becomes two. R (15..11) and G (10..5) go into separate
// fields of one word and are scaled together; B (4..0) is scaled on its
// own. Both factors already carry the triangle's own shade, folded in at
// setup, so this is the whole of the per-pixel cost.
static inline __attribute__((always_inline))
void scene_vrun_tex_tint_body(int lx, int y_top, int y_bot, ttri_setup_t const* st, bool const d16) {
    uint16_t const  frame = s_frame;
    uint32_t const  fhi   = (uint32_t)frame << 16;
    int const       idx   = rt_index(lx, y_top);
    uint16_t*       fp    = s_rt.fb + idx;
    uint32_t*       dp    = s_rt.ds + idx;
    uint16_t*       zp    = s_rt.dz + idx;
    float const     fx    = (float)lx, fy = (float)y_top;
    float           d     = st->Ad * fx + st->Bd * fy + st->Cd;
    float           us    = st->Au * fx + st->Bu * fy + st->Cu;
    float           vs    = st->Av * fx + st->Bv * fy + st->Cv;
    float const     Bd = st->Bd, Bu = st->Bu, Bv = st->Bv;
    uint16_t const* tx    = st->texels;
    uint32_t const  wm = st->wmask, hm = st->hmask, wl = st->wlog2;
    uint32_t const  shrg  = st->shade, shb = st->shade_b;
    bool const      rev   = s_rev;
    int             cnt   = y_bot - y_top + 1;
    s_rt.ttri_px += cnt;
    s_rt.ttri_sp++;
    while (cnt-- > 0) {
        int di = (int)d;
        if (di < 0) di = 0;
        uint16_t const stored =
            d16 ? *zp : ((uint16_t)(*dp >> 16) == frame) ? (uint16_t)*dp : 0;
        if ((uint16_t)di > stored) {
            if (d16) *zp = (uint16_t)di;
            else     *dp = fhi | (uint16_t)di;
            float const    inv = 1.0f / d;
            uint32_t const tu  = (uint32_t)(int)(us * inv) & wm;
            uint32_t const tv  = (uint32_t)(int)(vs * inv) & hm;
            uint32_t const t   = tx[(tv << wl) | tu];
            uint32_t       rg  = (t & 0xF800u) | ((t & 0x07E0u) << 16);
            rg                 = ((rg * shrg) >> 5) & 0x07E0F800u;
            uint32_t const bb  = (((t & 0x001Fu) * shb) >> 5) & 0x001Fu;
            uint16_t       px  = (uint16_t)(rg | (rg >> 16) | bb);
            if (rev) px = (uint16_t)((px >> 8) | (px << 8));
            *fp = px;
        }
        fp--;
        if (d16) zp--;
        else     dp--;
        d  += Bd;
        us += Bu;
        vs += Bv;
    }
}

// The same for a cut-out texture (se_texture.h): the texel is fetched
// before anything is written, and a hole (SE_TEXEL_CUTOUT) writes
// neither colour nor depth, so whatever lies behind shows through. A
// sibling rather than a flag in the loop above, so opaque textures run
// exactly the code they always did.
static inline __attribute__((always_inline))
void scene_vrun_tex_cutout_body(int lx, int y_top, int y_bot, ttri_setup_t const* st, bool const d16) {
    uint16_t const  frame = s_frame;
    uint32_t const  fhi   = (uint32_t)frame << 16;
    int const       idx   = rt_index(lx, y_top);
    uint16_t*       fp    = s_rt.fb + idx;
    uint32_t*       dp    = s_rt.ds + idx;
    uint16_t*       zp    = s_rt.dz + idx;
    float const     fx    = (float)lx, fy = (float)y_top;
    float           d     = st->Ad * fx + st->Bd * fy + st->Cd;
    float           us    = st->Au * fx + st->Bu * fy + st->Cu;
    float           vs    = st->Av * fx + st->Bv * fy + st->Cv;
    float const     Bd = st->Bd, Bu = st->Bu, Bv = st->Bv;
    uint16_t const* tx    = st->texels;
    uint32_t const  wm = st->wmask, hm = st->hmask, wl = st->wlog2;
    uint32_t const  sh    = st->shade;
    bool const      rev   = s_rev;
    int             cnt   = y_bot - y_top + 1;
    s_rt.ttri_px += cnt;
    s_rt.ttri_sp++;
    while (cnt-- > 0) {
        int di = (int)d;
        if (di < 0) di = 0;
        uint16_t const stored =
            d16 ? *zp : ((uint16_t)(*dp >> 16) == frame) ? (uint16_t)*dp : 0;
        if ((uint16_t)di > stored) {
            // di >= 1 here, so d >= 1: the divide is safe.
            float const    inv = 1.0f / d;
            uint32_t const tu  = (uint32_t)(int)(us * inv) & wm;
            uint32_t const tv  = (uint32_t)(int)(vs * inv) & hm;
            uint32_t const t   = tx[(tv << wl) | tu];
            if (t != SE_TEXEL_CUTOUT) {
                if (d16) *zp = (uint16_t)di;
                else     *dp = fhi | (uint16_t)di;
                uint32_t x = (t | (t << 16)) & 0x07E0F81Fu;
                x          = ((x * sh) >> 5) & 0x07E0F81Fu;
                uint16_t px = (uint16_t)(x | (x >> 16));
                if (rev) px = (uint16_t)((px >> 8) | (px << 8));
                *fp = px;
            }
        }
        fp--;
        if (d16) zp--;
        else     dp--;
        d  += Bd;
        us += Bu;
        vs += Bv;
    }
}

// The cut-out run, TINTED. See scene_vrun_tex_tint_body for the two
// multiplies; everything else is its plain sibling above, unchanged.
static inline __attribute__((always_inline))
void scene_vrun_tex_cutout_tint_body(int lx, int y_top, int y_bot, ttri_setup_t const* st, bool const d16) {
    uint16_t const  frame = s_frame;
    uint32_t const  fhi   = (uint32_t)frame << 16;
    int const       idx   = rt_index(lx, y_top);
    uint16_t*       fp    = s_rt.fb + idx;
    uint32_t*       dp    = s_rt.ds + idx;
    uint16_t*       zp    = s_rt.dz + idx;
    float const     fx    = (float)lx, fy = (float)y_top;
    float           d     = st->Ad * fx + st->Bd * fy + st->Cd;
    float           us    = st->Au * fx + st->Bu * fy + st->Cu;
    float           vs    = st->Av * fx + st->Bv * fy + st->Cv;
    float const     Bd = st->Bd, Bu = st->Bu, Bv = st->Bv;
    uint16_t const* tx    = st->texels;
    uint32_t const  wm = st->wmask, hm = st->hmask, wl = st->wlog2;
    uint32_t const  shrg  = st->shade, shb = st->shade_b;
    bool const      rev   = s_rev;
    int             cnt   = y_bot - y_top + 1;
    s_rt.ttri_px += cnt;
    s_rt.ttri_sp++;
    while (cnt-- > 0) {
        int di = (int)d;
        if (di < 0) di = 0;
        uint16_t const stored =
            d16 ? *zp : ((uint16_t)(*dp >> 16) == frame) ? (uint16_t)*dp : 0;
        if ((uint16_t)di > stored) {
            // di >= 1 here, so d >= 1: the divide is safe.
            float const    inv = 1.0f / d;
            uint32_t const tu  = (uint32_t)(int)(us * inv) & wm;
            uint32_t const tv  = (uint32_t)(int)(vs * inv) & hm;
            uint32_t const t   = tx[(tv << wl) | tu];
            if (t != SE_TEXEL_CUTOUT) {
                if (d16) *zp = (uint16_t)di;
                else     *dp = fhi | (uint16_t)di;
                uint32_t rg = (t & 0xF800u) | ((t & 0x07E0u) << 16);
                rg          = ((rg * shrg) >> 5) & 0x07E0F800u;
                uint32_t const bb = (((t & 0x001Fu) * shb) >> 5) & 0x001Fu;
                uint16_t px = (uint16_t)(rg | (rg >> 16) | bb);
                if (rev) px = (uint16_t)((px >> 8) | (px << 8));
                *fp = px;
            }
        }
        fp--;
        if (d16) zp--;
        else     dp--;
        d  += Bd;
        us += Bu;
        vs += Bv;
    }
}

static inline void scene_vrun_tex(int lx, int y_top, int y_bot, ttri_setup_t const* st) {
    if (lx < s_rt.vp_x0 || lx > s_rt.vp_x1) return;
    if (y_top < s_rt.vp_y0) y_top = s_rt.vp_y0;
    if (y_bot > s_rt.vp_y1) y_bot = s_rt.vp_y1;
    if (y_top > y_bot) return;
    if (s_tint_on) {
        if (s_rt.dz_on) scene_vrun_tex_tint_body(lx, y_top, y_bot, st, true);
        else         scene_vrun_tex_tint_body(lx, y_top, y_bot, st, false);
        return;
    }
    if (s_rt.dz_on) scene_vrun_tex_body(lx, y_top, y_bot, st, true);
    else         scene_vrun_tex_body(lx, y_top, y_bot, st, false);
}

static inline void scene_vrun_tex_cutout(int lx, int y_top, int y_bot, ttri_setup_t const* st) {
    if (lx < s_rt.vp_x0 || lx > s_rt.vp_x1) return;
    if (y_top < s_rt.vp_y0) y_top = s_rt.vp_y0;
    if (y_bot > s_rt.vp_y1) y_bot = s_rt.vp_y1;
    if (y_top > y_bot) return;
    if (s_tint_on) {
        if (s_rt.dz_on) scene_vrun_tex_cutout_tint_body(lx, y_top, y_bot, st, true);
        else         scene_vrun_tex_cutout_tint_body(lx, y_top, y_bot, st, false);
        return;
    }
    if (s_rt.dz_on) scene_vrun_tex_cutout_body(lx, y_top, y_bot, st, true);
    else         scene_vrun_tex_cutout_body(lx, y_top, y_bot, st, false);
}

static void scene_raster_ttri(se_ttri_t const* t) {
    se_tvtx_t const a = t->v[0], b = t->v[1], c = t->v[2];
    float const ex1 = b.sx - a.sx, ey1 = b.sy - a.sy;
    float const ex2 = c.sx - a.sx, ey2 = c.sy - a.sy;
    float const nz  = ex1 * ey2 - ey1 * ex2;
    if (nz > -1e-6f && nz < 1e-6f) return;   // zero screen area
    float const inv_nz = 1.0f / nz;

    // Plane through (sx, sy, q) for each interpolated quantity -- the
    // same construction scene_raster_tri uses for its depth plane.
    ttri_setup_t st;
#define TTRI_PLANE(qa, qb, qc, A, B, C)                          \
    do {                                                        \
        float const eq1 = ((qb) - (qa)) * SCENE_DEPTH_SCALE;    \
        float const eq2 = ((qc) - (qa)) * SCENE_DEPTH_SCALE;    \
        (A) = (eq1 * ey2 - ey1 * eq2) * inv_nz;                 \
        (B) = (ex1 * eq2 - eq1 * ex2) * inv_nz;                 \
        (C) = (qa) * SCENE_DEPTH_SCALE - (A) * a.sx - (B) * a.sy; \
    } while (0)
    TTRI_PLANE(a.w, b.w, c.w, st.Ad, st.Bd, st.Cd);
    TTRI_PLANE(a.uw, b.uw, c.uw, st.Au, st.Bu, st.Cu);
    TTRI_PLANE(a.vw, b.vw, c.vw, st.Av, st.Bv, st.Cv);
#undef TTRI_PLANE
    st.texels = t->tex->texels;
    st.wmask  = (uint32_t)t->tex->w - 1u;
    st.hmask  = (uint32_t)t->tex->h - 1u;
    st.wlog2  = t->tex->w_log2;
    // The tint rides on the triangle's own shade, so the inner loop pays
    // two multiplies and no arithmetic beyond them (se_scene.h).
    st.shade   = (uint32_t)t->shade * s_tint_rg / 32u;
    st.shade_b = (uint32_t)t->shade * s_tint_b / 32u;
    bool const cutout = t->tex->cutout;

    // Column scan, as in scene_raster_tri.
    float x0 = a.sx, y0 = a.sy, x1 = b.sx, y1 = b.sy, x2 = c.sx, y2 = c.sy;
    float tx, ty;
    if (x1 < x0) { tx=x0; ty=y0; x0=x1; y0=y1; x1=tx; y1=ty; }
    if (x2 < x0) { tx=x0; ty=y0; x0=x2; y0=y2; x2=tx; y2=ty; }
    if (x2 < x1) { tx=x1; ty=y1; x1=x2; y1=y2; x2=tx; y2=ty; }

    if (x2 < (float)s_rt.vp_x0 || x0 > (float)s_rt.vp_x1) return;
    if (x2 - x0 < 1e-6f) return;

    float const dydx_02 = (y2 - y0) / (x2 - x0);
    float const dydx_01 = (x1 > x0) ? (y1 - y0) / (x1 - x0) : 0.0f;
    float const dydx_12 = (x2 > x1) ? (y2 - y1) / (x2 - x1) : 0.0f;

    int ix_start =     ceil_i(x0);
    int ix_split =     ceil_i(x1);
    int ix_endex = 1 + floor_i(x2);
    if (ix_start < s_rt.vp_x0)      ix_start = s_rt.vp_x0;
    if (ix_endex > s_rt.vp_x1 + 1)  ix_endex = s_rt.vp_x1 + 1;
    if (ix_split < ix_start)      ix_split = ix_start;
    if (ix_split > ix_endex)      ix_split = ix_endex;

    for (int x = ix_start; x < ix_split; x++) {
        float const dx = (float)x - x0;
        float const ya = y0 + dydx_02 * dx;
        float const yb = y0 + dydx_01 * dx;
        float yt, yz;
        if (ya < yb) { yt = ya; yz = yb; } else { yt = yb; yz = ya; }
        if (cutout) scene_vrun_tex_cutout(x, ceil_i(yt), floor_i(yz), &st);
        else        scene_vrun_tex(x, ceil_i(yt), floor_i(yz), &st);
    }
    for (int x = ix_split; x < ix_endex; x++) {
        float const dx02 = (float)x - x0;
        float const dx12 = (float)x - x1;
        float const ya   = y0 + dydx_02 * dx02;
        float const yb   = y1 + dydx_12 * dx12;
        float yt, yz;
        if (ya < yb) { yt = ya; yz = yb; } else { yt = yb; yz = ya; }
        if (cutout) scene_vrun_tex_cutout(x, ceil_i(yt), floor_i(yz), &st);
        else        scene_vrun_tex(x, ceil_i(yt), floor_i(yz), &st);
    }
}

void se_scene_raster_textured(void) {
    if (s_ttris == NULL || s_fb == NULL || s_ds == NULL) return;
    int64_t const t0 = esp_timer_get_time();
    for (int i = 0; i < s_ttri_n; i++) {
        scene_raster_ttri(&s_ttris[i]);
    }
    s_stat_ttri_us = esp_timer_get_time() - t0;
}

// --- Line rasterizer ----------------------------------------------------------

// Depth-tested wireframe edge. Bresenham line with encoded depth
// interpolated along it; tests the depth buffer (with the
// SCENE_LINE_BIAS nudge baked into the endpoint depths) but never
// writes it — an edge is an overlay, not a depth occluder.
static void scene_raster_line(scene_vtx_t a, scene_vtx_t b, uint16_t packed) {
    int const x0 = (int)lroundf(a.sx), y0 = (int)lroundf(a.sy);
    int const x1 = (int)lroundf(b.sx), y1 = (int)lroundf(b.sy);

    int const dx = abs(x1 - x0);
    int const dy = abs(y1 - y0);
    int const sx = (x0 < x1) ? 1 : -1;
    int const sy = (y0 < y1) ? 1 : -1;
    int       err = dx - dy;

    int   const steps = (dx > dy) ? dx : dy;
    float const eda   = a.w * (SCENE_LINE_BIAS * SCENE_DEPTH_SCALE);
    float const edb   = b.w * (SCENE_LINE_BIAS * SCENE_DEPTH_SCALE);
    float       d     = eda;
    float const dd    = (steps > 0) ? (edb - eda) / (float)steps : 0.0f;

    uint16_t const frame = s_frame;
    int     const ptr_dx = (sx > 0) ? s_raw_stride : -s_raw_stride;
    int     const ptr_dy = (sy > 0) ? -1 : 1;

    int const idx = rt_index(x0, y0);
    uint16_t* fp  = s_rt.fb + idx;
    uint32_t* dp  = s_rt.ds + idx;
    uint16_t* zp  = s_rt.dz + idx;
    bool const d16 = s_rt.dz_on;
    int       lx  = x0;
    int       ly  = y0;

    while (1) {
        if (lx >= s_rt.vp_x0 && lx <= s_rt.vp_x1 && ly >= s_rt.vp_y0 && ly <= s_rt.vp_y1) {
            int di = (int)d;
            if (di < 0) di = 0;
            uint16_t const stored =
                d16 ? *zp : ((uint16_t)(*dp >> 16) == frame) ? (uint16_t)*dp : 0;
            if ((uint16_t)di >= stored) *fp = packed;   // edges test depth but never write it
        }
        if (lx == x1 && ly == y1) break;
        int const e2 = 2 * err;
        if (e2 > -dy) { err -= dy; lx += sx; fp += ptr_dx; dp += ptr_dx; zp += ptr_dx; }
        if (e2 <  dx) { err += dx; ly += sy; fp += ptr_dy; dp += ptr_dy; zp += ptr_dy; }
        d += dd;
    }
}

// --- Near-plane clipping ------------------------------------------------------
//
// A primitive that lies partly behind the near plane is CLIPPED to it,
// not squashed: the part in front is kept at its true shape, the part
// behind is cut away. (Clamping each behind-plane vertex onto the plane,
// as the projection guard in scene_project_cam would, keeps its x/y and
// so bends the triangle out of shape -- and its texture with it -- the
// moment a camera flies close past geometry.) Primitives entirely in
// front take the unchanged fast path, so their output is bit-identical
// to what it was before clipping existed.

// A camera-space vertex plus the texture coordinate the clip carries
// along (unused, and left at 0, for flat triangles).
typedef struct {
    float x, y, z;
    float u, v;
} clip_vtx_t;

// Clip a camera-space triangle against z >= RENDER_NEAR_CLIP_Z (one
// Sutherland-Hodgman pass). Writes the surviving polygon to `out`, in the
// same winding order, and returns its vertex count: 3 when two vertices
// were behind, 4 when one was, 0 when all three were. Each new vertex
// lies exactly on the plane, interpolated along the edge it cuts;
// position and texture coordinate are both affine along a camera-space
// edge, so the interpolation is exact.
static int clip_near(clip_vtx_t const in[3], clip_vtx_t out[4]) {
    int n = 0;
    for (int i = 0; i < 3; i++) {
        clip_vtx_t const* a    = &in[i];
        clip_vtx_t const* b    = &in[i == 2 ? 0 : i + 1];
        bool const        a_in = a->z >= RENDER_NEAR_CLIP_Z;
        bool const        b_in = b->z >= RENDER_NEAR_CLIP_Z;
        if (a_in) out[n++] = *a;
        if (a_in != b_in) {
            float const t = (RENDER_NEAR_CLIP_Z - a->z) / (b->z - a->z);
            out[n++] = (clip_vtx_t){
                a->x + (b->x - a->x) * t, a->y + (b->y - a->y) * t, RENDER_NEAR_CLIP_Z,
                a->u + (b->u - a->u) * t, a->v + (b->v - a->v) * t,
            };
        }
    }
    return n;
}

static inline int clip_behind_count(clip_vtx_t const c[3]) {
    return (c[0].z < RENDER_NEAR_CLIP_Z) + (c[1].z < RENDER_NEAR_CLIP_Z) + (c[2].z < RENDER_NEAR_CLIP_Z);
}

// --- Public submit / flush ----------------------------------------------------

// Append one projected flat triangle (all vertices at or in front of the
// near plane).
static void emit_tri(clip_vtx_t const* a, clip_vtx_t const* b, clip_vtx_t const* c, uint16_t packed) {
    if (s_tri_n >= SCENE_TRI_CAP) {
        s_stat_tri_drop++;
        return;  // overflow: drop extra tris
    }
    scene_tri_t* t = &s_tris[s_tri_n++];
    scene_project_cam(a->x, a->y, a->z, &t->v[0]);
    scene_project_cam(b->x, b->y, b->z, &t->v[1]);
    scene_project_cam(c->x, c->y, c->z, &t->v[2]);
    t->packed = packed;
}

void scene_tri(float x0, float y0, float z0,
               float x1, float y1, float z1,
               float x2, float y2, float z2, uint32_t argb, uint32_t flags) {
    if (!s_tris) return;
    clip_vtx_t c[3] = {{0}};
    camera_transform(x0, y0, z0, &c[0].x, &c[0].y, &c[0].z);
    camera_transform(x1, y1, z1, &c[1].x, &c[1].y, &c[1].z);
    camera_transform(x2, y2, z2, &c[2].x, &c[2].y, &c[2].z);
    // Entirely behind the near plane (in CAMERA space, so this is right
    // under any camera pose): nothing to draw. This is not the central
    // frustum cull (scene_cull_pass, opt-in via scene_set_options).
    int const behind = clip_behind_count(c);
    if (behind == 3) return;
    if (behind == 0 && s_tri_n >= SCENE_TRI_CAP) {   // overflow: skip the shading too
        s_stat_tri_drop++;  // counted here too -- the common case, and the one that went unseen
        return;
    }

    // Lighting (se_light.h): shade the face once, from the WORLD-space
    // vertices -- the light lives in world space -- whatever clipping does
    // to it below: the pieces of a clipped face share its normal. Off by
    // default, and then this is a load and a branch. An emissive triangle
    // skips it and keeps the colour it was given.
    if (se_light_is_on && !(flags & SE_TRI_EMISSIVE)) {
        argb = se_light_shade_tri(argb, x0, y0, z0, x1, y1, z1, x2, y2, z2,
                                  s_camera.x, s_camera.y, s_camera.z);
    }
    // The scene tint (se_scene.h), folded into the colour here -- a flat
    // triangle is shaded once, so tinting one costs nothing per pixel.
    if (s_tint_on) {
        uint32_t const r = ((argb >> 16) & 0xFFu) * s_tint_rg / 32u;
        uint32_t const g = ((argb >> 8) & 0xFFu) * s_tint_rg / 32u;
        uint32_t const b = (argb & 0xFFu) * s_tint_b / 32u;
        argb             = (argb & 0xFF000000u) | (r << 16) | (g << 8) | b;
    }
    // The game's own light level (SE_TRI_LIGHT), folded into the colour.
    if (flags & SE_TRI_LIGHT_MASK) {
        uint32_t const lv = se_tri_light_level(flags);
        uint32_t const r  = ((argb >> 16) & 0xFFu) * lv / SE_TRI_LIGHT_MAX;
        uint32_t const g  = ((argb >> 8) & 0xFFu) * lv / SE_TRI_LIGHT_MAX;
        uint32_t const b  = (argb & 0xFFu) * lv / SE_TRI_LIGHT_MAX;
        argb              = (argb & 0xFF000000u) | (r << 16) | (g << 8) | b;
    }
    uint16_t const packed = direct_565_pack(argb, s_rev);

    if (behind == 0) {
        emit_tri(&c[0], &c[1], &c[2], packed);
        return;
    }
    clip_vtx_t p[4];
    int const  n = clip_near(c, p);
    emit_tri(&p[0], &p[1], &p[2], packed);
    if (n == 4) emit_tri(&p[0], &p[2], &p[3], packed);
}

void scene_line(float x0, float y0, float z0,
                float x1, float y1, float z1, uint32_t argb) {
    if (!s_lines) return;
    float c0x, c0y, c0z, c1x, c1y, c1z;
    camera_transform(x0, y0, z0, &c0x, &c0y, &c0z);
    camera_transform(x1, y1, z1, &c1x, &c1y, &c1z);
    bool const in0 = c0z >= RENDER_NEAR_CLIP_Z;
    bool const in1 = c1z >= RENDER_NEAR_CLIP_Z;
    if (!in0 && !in1) return;
    if (s_line_n >= SCENE_LINE_CAP) return;
    if (!in0 || !in1) {
        // Crossing the near plane: move the endpoint behind it onto the
        // plane, along the line.
        float const t = (RENDER_NEAR_CLIP_Z - c0z) / (c1z - c0z);
        float const x = c0x + (c1x - c0x) * t;
        float const y = c0y + (c1y - c0y) * t;
        if (in0) {
            c1x = x; c1y = y; c1z = RENDER_NEAR_CLIP_Z;
        } else {
            c0x = x; c0y = y; c0z = RENDER_NEAR_CLIP_Z;
        }
    }
    scene_seg_t* seg = &s_lines[s_line_n++];
    scene_project_cam(c0x, c0y, c0z, &seg->v[0]);
    scene_project_cam(c1x, c1y, c1z, &seg->v[1]);
    seg->packed = direct_565_pack(argb, s_rev);
}

// Append one projected textured triangle (all vertices at or in front of
// the near plane). `ush` / `vsh` are the whole texture periods the
// original triangle's coordinates were shifted by (see below), so every
// piece of a clipped triangle maps the texture exactly as the whole did.
static void emit_ttri(clip_vtx_t const* a, clip_vtx_t const* b, clip_vtx_t const* c,
                      se_texture_t const* tex, float ush, float vsh, uint8_t shade) {
    if (s_ttri_n >= SE_SCENE_TEXTURED_TRI_CAP) {
        s_stat_ttri_drop++;
        return;  // overflow: drop, as scene_tri does
    }
    se_ttri_t* t = &s_ttris[s_ttri_n++];
    clip_vtx_t const* const q[3] = {a, b, c};
    float const tw = (float)tex->w, th = (float)tex->h;
    for (int i = 0; i < 3; i++) {
        scene_vtx_t p;
        scene_project_cam(q[i]->x, q[i]->y, q[i]->z, &p);
        t->v[i].sx = p.sx;
        t->v[i].sy = p.sy;
        t->v[i].w  = p.w;
        t->v[i].uw = (q[i]->u - ush) * tw * p.w;
        t->v[i].vw = (q[i]->v - vsh) * th * p.w;
    }
    t->tex   = tex;
    t->shade = shade;
}

void scene_textured_tri(se_tex_vertex_t const v[3], se_texture_t const* tex, uint32_t flags) {
    if (v == NULL || tex == NULL || tex->texels == NULL) return;
    if (s_ttris == NULL && !scene_textured_reserve()) return;
    clip_vtx_t c[3];
    for (int i = 0; i < 3; i++) {
        camera_transform(v[i].x, v[i].y, v[i].z, &c[i].x, &c[i].y, &c[i].z);
        c[i].u = v[i].u;
        c[i].v = v[i].v;
    }
    // Same near handling as scene_tri.
    int const behind = clip_behind_count(c);
    if (behind == 3) return;
    if (behind == 0 && s_ttri_n >= SE_SCENE_TEXTURED_TRI_CAP) {
        s_stat_ttri_drop++;  // counted here too -- the common case, and the one that went unseen
        return;
    }

    // Shift the coordinates by whole texture periods so all three are
    // >= 0. Repeating makes that invisible, and it lets the rasterizer
    // turn a texel coordinate into an index by truncating to int, which
    // only rounds the right way (down) for non-negative values. Taken
    // from the ORIGINAL corners: a clipped piece's coordinates are
    // interpolated between them, so they stay >= the same minimum.
    float umin = v[0].u, vmin = v[0].v;
    if (v[1].u < umin) umin = v[1].u;
    if (v[2].u < umin) umin = v[2].u;
    if (v[1].v < vmin) vmin = v[1].v;
    if (v[2].v < vmin) vmin = v[2].v;
    float const ush = floorf(umin), vsh = floorf(vmin);

    // Lighting (se_light.h): the same per-face shade scene_tri applies,
    // from the same world-space vertices, but kept as a factor because
    // there is no single colour to fold it into. Emissive: full strength.
    uint8_t shade = 32;
    if (se_light_is_on && !(flags & SE_TRI_EMISSIVE)) {
        float const sh = se_light_face_shade(v[0].x, v[0].y, v[0].z, v[1].x, v[1].y, v[1].z,
                                             v[2].x, v[2].y, v[2].z, s_camera.x, s_camera.y, s_camera.z);
        shade = (uint8_t)(sh * 32.0f + 0.5f);
    }
    // The game's own light level (SE_TRI_LIGHT), multiplied in.
    if (flags & SE_TRI_LIGHT_MASK) shade = (uint8_t)((uint32_t)shade * se_tri_light_level(flags) / SE_TRI_LIGHT_MAX);

    if (behind == 0) {
        emit_ttri(&c[0], &c[1], &c[2], tex, ush, vsh, shade);
        return;
    }
    clip_vtx_t p[4];
    int const  n = clip_near(c, p);
    emit_ttri(&p[0], &p[1], &p[2], tex, ush, vsh, shade);
    if (n == 4) emit_ttri(&p[0], &p[2], &p[3], tex, ush, vsh, shade);
}

// --- Points -------------------------------------------------------------------

static bool scene_points_reserve(void) {
    if (s_pts != NULL) return true;
    if (s_pt_failed) return false;
    size_t const sz = (size_t)SE_SCENE_POINT_CAP * sizeof(se_pt_t);
    s_pts = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (s_pts == NULL) {
        ESP_LOGE(TAG, "point list allocation failed (%u bytes)", (unsigned)sz);
        s_pt_failed = true;
        return false;
    }
    ESP_LOGI(TAG, "point list: PSRAM (%uKB, %d points)", (unsigned)(sz / 1024), SE_SCENE_POINT_CAP);
    return true;
}

void scene_point(float x, float y, float z, uint32_t argb) {
    if (s_pts == NULL && !scene_points_reserve()) return;
    float cx, cy, cz;
    camera_transform(x, y, z, &cx, &cy, &cz);
    if (cz < RENDER_NEAR_CLIP_Z) return;
    scene_vtx_t p;
    scene_project_cam(cx, cy, cz, &p);
    // One pixel has nothing to clip: cull it here, against the viewport,
    // instead of carrying it through the cull pass.
    int const px = (int)lroundf(p.sx), py = (int)lroundf(p.sy);
    if (px < s_vp_x0 || px > s_vp_x1 || py < s_vp_y0 || py > s_vp_y1) return;
    if (s_pt_n >= SE_SCENE_POINT_CAP) return;   // overflow: drop, as the other lists do
    se_pt_t* pt = &s_pts[s_pt_n++];
    pt->v       = p;
    pt->packed  = direct_565_pack(argb, s_rev);
}

// Depth-tested, never depth-writing single pixels (as the edges, without
// their bias: a point is not drawn over a face it belongs to).
static inline void scene_raster_point(se_pt_t const* pt) {
    int const x = (int)lroundf(pt->v.sx), y = (int)lroundf(pt->v.sy);
    if (x < s_rt.vp_x0 || x > s_rt.vp_x1 || y < s_rt.vp_y0 || y > s_rt.vp_y1) return;
    int const      idx    = rt_index(x, y);
    uint16_t const stored =
        s_rt.dz_on ? s_rt.dz[idx] : ((uint16_t)(s_rt.ds[idx] >> 16) == s_frame) ? (uint16_t)s_rt.ds[idx] : 0;
    int di = (int)(pt->v.w * SCENE_DEPTH_SCALE);
    if (di < 0) di = 0;
    if ((uint16_t)di >= stored) s_rt.fb[idx] = pt->packed;
}

void se_scene_raster_points(void) {
    if (s_pts == NULL || s_fb == NULL || s_ds == NULL) return;
    int64_t const t0 = esp_timer_get_time();
    for (int i = 0; i < s_pt_n; i++) {
        scene_raster_point(&s_pts[i]);
    }
    s_stat_pt_us = esp_timer_get_time() - t0;
}

// --- Deferred render: cull -> order -> rasterize ------------------------------
//
// Central cull / order passes, both opt-in via scene_set_options() and
// both output-neutral: they change only how fast scene_render() produces
// the SAME image, never the image itself. With both off (the default) the
// pipeline is byte-identical to the original hybrid-immediate path. Each
// runs against the already-projected geometry, so it respects the camera
// pose + FOV for free WITHOUT touching any game submit call site.
//
// NB: back-face culling is deliberately NOT here. The engine only sees
// anonymous projected triangles; the game's objects know their face
// normals and already cull back faces at emit time (e.g. render.c's
// emit_cube), which is both cheaper and safe regardless of winding.

void scene_set_options(se_scene_options_t const* opts) {
    if (opts == NULL) {
        s_opts.frustum_cull = false;
        s_opts.depth_order  = false;
    } else {
        s_opts = *opts;
    }
}

se_scene_options_t scene_get_options(void) {
    return s_opts;
}

// A primitive is off-screen iff all its vertices lie outside the SAME
// screen edge (convex-hull argument: the whole primitive is then in that
// half-plane and covers no on-screen pixel). Conservative — a primitive
// straddling a corner off-screen is not caught here, but the per-pixel
// clip in scene_vrun / scene_raster_line handles that for free. The
// screen rect is the projected frustum's four side planes, so this is
// frustum culling that already accounts for the camera pose and FOV.
static inline bool tri_offscreen(scene_tri_t const* t) {
    float const L = (float)s_vp_x0, R = (float)s_vp_x1;
    float const T = (float)s_vp_y0, B = (float)s_vp_y1;
    if (t->v[0].sx < L && t->v[1].sx < L && t->v[2].sx < L) return true;
    if (t->v[0].sx > R && t->v[1].sx > R && t->v[2].sx > R) return true;
    if (t->v[0].sy < T && t->v[1].sy < T && t->v[2].sy < T) return true;
    if (t->v[0].sy > B && t->v[1].sy > B && t->v[2].sy > B) return true;
    return false;
}

static inline bool ttri_offscreen(se_ttri_t const* t) {
    float const L = (float)s_vp_x0, R = (float)s_vp_x1;
    float const T = (float)s_vp_y0, B = (float)s_vp_y1;
    if (t->v[0].sx < L && t->v[1].sx < L && t->v[2].sx < L) return true;
    if (t->v[0].sx > R && t->v[1].sx > R && t->v[2].sx > R) return true;
    if (t->v[0].sy < T && t->v[1].sy < T && t->v[2].sy < T) return true;
    if (t->v[0].sy > B && t->v[1].sy > B && t->v[2].sy > B) return true;
    return false;
}

static inline bool seg_offscreen(scene_seg_t const* s) {
    float const L = (float)s_vp_x0, R = (float)s_vp_x1;
    float const T = (float)s_vp_y0, B = (float)s_vp_y1;
    if (s->v[0].sx < L && s->v[1].sx < L) return true;
    if (s->v[0].sx > R && s->v[1].sx > R) return true;
    if (s->v[0].sy < T && s->v[1].sy < T) return true;
    if (s->v[0].sy > B && s->v[1].sy > B) return true;
    return false;
}

// Frustum cull: compact off-screen primitives out of the triangle and
// edge lists in place (stable, preserving relative order). Survivors
// rasterize unchanged; dropped primitives covered zero pixels, so the
// output is identical — only the per-primitive setup work is saved.
static void scene_cull_pass(void) {
    if (!s_opts.frustum_cull) return;
    int w = 0;
    for (int i = 0; i < s_tri_n; i++) {
        if (!tri_offscreen(&s_tris[i])) {
            if (w != i) s_tris[w] = s_tris[i];
            w++;
        }
    }
    s_tri_n = w;
    w = 0;
    for (int i = 0; i < s_line_n; i++) {
        if (!seg_offscreen(&s_lines[i])) {
            if (w != i) s_lines[w] = s_lines[i];
            w++;
        }
    }
    s_line_n = w;
    w = 0;
    for (int i = 0; i < s_ttri_n; i++) {
        if (!ttri_offscreen(&s_ttris[i])) {
            if (w != i) s_ttris[w] = s_ttris[i];
            w++;
        }
    }
    s_ttri_n = w;
}

// Front-to-back triangle comparator: nearer first. Vertex w is 1/z
// (larger = nearer); the sum of the three w's orders by inverse centroid
// depth without a divide. Ties keep an arbitrary order — harmless, since
// the per-pixel z-test resolves same-depth triangles either way.
static int tri_cmp_near_first(void const* pa, void const* pb) {
    scene_tri_t const* a = (scene_tri_t const*)pa;
    scene_tri_t const* b = (scene_tri_t const*)pb;
    float const wa = a->v[0].w + a->v[1].w + a->v[2].w;
    float const wb = b->v[0].w + b->v[1].w + b->v[2].w;
    if (wa > wb) return -1;   // a is nearer -> rasterize earlier
    if (wa < wb) return 1;
    return 0;
}

// Depth order: sort triangles front-to-back so occluded fragments fail
// the depth test with no framebuffer write (early-z). The final depth
// buffer is order-independent (max-wins per pixel), so the image is
// identical; only the count of framebuffer writes changes. Edges are
// never sorted — they don't write depth, so their order can't matter.
static int ttri_cmp_near_first(void const* pa, void const* pb) {
    se_ttri_t const* a = (se_ttri_t const*)pa;
    se_ttri_t const* b = (se_ttri_t const*)pb;
    float const wa = a->v[0].w + a->v[1].w + a->v[2].w;
    float const wb = b->v[0].w + b->v[1].w + b->v[2].w;
    if (wa > wb) return -1;
    if (wa < wb) return 1;
    return 0;
}

// The sort itself is a KEY sort, not a qsort of the lists: qsort moves
// 40- and 68-byte records around with a comparator call per compare,
// and when the lists live in PSRAM (SE_SCENE_DEPTH16_INTERNAL pushes
// them there) that cost the far view 5 ms a frame. Instead each
// triangle gets one uint32 in internal SRAM -- a 16-bit depth key over
// its 16-bit index -- the keys are radix sorted (two 8-bit passes, on
// the key half only; stable), and the records are then gathered once,
// in order, into a second buffer that swaps places with the first.
//
// The depth key is the top 16 bits of the float w-sum (sign, exponent,
// 7 mantissa bits): positive floats order like their bit patterns, so
// that is the same order to within 1 part in 128, and a tie is harmless
// (see tri_cmp_near_first). Inverted, so nearer sorts first.
//
// Without room for the keys or the second buffer, the qsort stays.
_Static_assert(SE_SCENE_TRI_CAP <= 65536 && SE_SCENE_TEXTURED_TRI_CAP <= 65536,
               "the key sort keeps a triangle's index in 16 bits");
#define ORDER_KEY_CAP (SE_SCENE_TRI_CAP > SE_SCENE_TEXTURED_TRI_CAP ? SE_SCENE_TRI_CAP : SE_SCENE_TEXTURED_TRI_CAP)
static uint32_t*    s_ok       = NULL;   // keys, internal SRAM
static uint32_t*    s_ok_tmp   = NULL;   // radix scratch, internal SRAM
static scene_tri_t* s_tris_alt = NULL;   // gather target, swaps with s_tris
static se_ttri_t*   s_ttris_alt = NULL;  // ... and with s_ttris
static bool         s_ok_failed = false;

static bool order_keys_reserve(void) {
    if (s_ok != NULL) return true;
    if (s_ok_failed) return false;
    s_ok     = heap_caps_malloc(ORDER_KEY_CAP * sizeof(uint32_t), MALLOC_CAP_INTERNAL);
    s_ok_tmp = heap_caps_malloc(ORDER_KEY_CAP * sizeof(uint32_t), MALLOC_CAP_INTERNAL);
    if (s_ok == NULL || s_ok_tmp == NULL) {
        ESP_LOGW(TAG, "no internal SRAM for depth-order keys (%uKB): sorting with qsort",
                 (unsigned)(2 * ORDER_KEY_CAP * sizeof(uint32_t) / 1024));
        free(s_ok);
        free(s_ok_tmp);
        s_ok = s_ok_tmp = NULL;
        s_ok_failed = true;
        return false;
    }
    return true;
}

// Allocate the gather buffer for a list like `list` -- in the same kind
// of memory, so the swap never moves a list somewhere slower.
static void* order_alt_alloc(void const* list, size_t sz) {
    uint32_t const caps = esp_ptr_internal(list) ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM;
    return heap_caps_malloc(sz, caps);
}

static inline uint32_t order_key(float wsum, int i) {
    uint32_t bits;
    memcpy(&bits, &wsum, sizeof(bits));
    if ((int32_t)bits < 0) bits = 0;   // a negative w-sum cannot happen; sort it last
    return ((0xFFFFu - (bits >> 16)) << 16) | (uint32_t)i;
}

// Stable LSD radix sort of s_ok[0..n) on bits 16..31.
static void order_radix(int n) {
    uint32_t* src = s_ok;
    uint32_t* dst = s_ok_tmp;
    for (int shift = 16; shift < 32; shift += 8) {
        uint32_t count[256] = {0};
        for (int i = 0; i < n; i++) count[(src[i] >> shift) & 0xFFu]++;
        uint32_t sum = 0;
        for (int b = 0; b < 256; b++) {
            uint32_t const c = count[b];
            count[b] = sum;
            sum += c;
        }
        for (int i = 0; i < n; i++) dst[count[(src[i] >> shift) & 0xFFu]++] = src[i];
        uint32_t* const t = src;
        src = dst;
        dst = t;
    }
    // Two passes: the sorted keys are back in s_ok.
}

static void scene_order_pass(void) {
    if (!s_opts.depth_order) return;
    bool const keys = order_keys_reserve();
    if (s_tri_n > 1) {
        if (keys && s_tris_alt == NULL) s_tris_alt = order_alt_alloc(s_tris, (size_t)SCENE_TRI_CAP * sizeof(scene_tri_t));
        if (keys && s_tris_alt != NULL) {
            for (int i = 0; i < s_tri_n; i++) {
                scene_tri_t const* t = &s_tris[i];
                s_ok[i] = order_key(t->v[0].w + t->v[1].w + t->v[2].w, i);
            }
            order_radix(s_tri_n);
            for (int i = 0; i < s_tri_n; i++) s_tris_alt[i] = s_tris[s_ok[i] & 0xFFFFu];
            scene_tri_t* const t = s_tris;
            s_tris     = s_tris_alt;
            s_tris_alt = t;
        } else {
            qsort(s_tris, (size_t)s_tri_n, sizeof(scene_tri_t), tri_cmp_near_first);
        }
    }
    // Sorted within their own list: the two lists rasterize one after
    // the other, so they cannot interleave. Early-z pays off more here
    // than for flat triangles, since an occluded textured pixel skips a
    // divide and a texel fetch as well as the framebuffer write.
    if (s_ttri_n > 1) {
        if (keys && s_ttris_alt == NULL) {
            s_ttris_alt = order_alt_alloc(s_ttris, (size_t)SE_SCENE_TEXTURED_TRI_CAP * sizeof(se_ttri_t));
        }
        if (keys && s_ttris_alt != NULL) {
            for (int i = 0; i < s_ttri_n; i++) {
                se_ttri_t const* t = &s_ttris[i];
                s_ok[i] = order_key(t->v[0].w + t->v[1].w + t->v[2].w, i);
            }
            order_radix(s_ttri_n);
            for (int i = 0; i < s_ttri_n; i++) s_ttris_alt[i] = s_ttris[s_ok[i] & 0xFFFFu];
            se_ttri_t* const t = s_ttris;
            s_ttris     = s_ttris_alt;
            s_ttris_alt = t;
        } else {
            qsort(s_ttris, (size_t)s_ttri_n, sizeof(se_ttri_t), ttri_cmp_near_first);
        }
    }
}

// =====================================================================
//  The built-in renderer -- SE_RENDER_ZBUFFER (primitive-driven)
// ---------------------------------------------------------------------
//  The original pipeline, unchanged: walk the triangles in list order and
//  scan-convert each one, depth-testing per covered pixel. Cost scales
//  with summed triangle area, so a pixel under N overlapping triangles is
//  visited N times (overdraw). The central cull / order passes have
//  already run, so prepare() has nothing left to do.
// =====================================================================

static void zbuf_prepare(void* user) {
    (void)user;   // cull + order run centrally, before the renderer's prepare
}

static void zbuf_rasterize(void* user) {
    (void)user;
    int64_t const t_r0 = esp_timer_get_time();
    // Triangles first (per-pixel z-test makes their order irrelevant), in
    // submission order -- identical to the old immediate path.
    for (int i = 0; i < s_tri_n; i++) {
        scene_raster_tri(s_tris[i].v[0], s_tris[i].v[1], s_tris[i].v[2], s_tris[i].packed);
    }
    int64_t const t_r1 = esp_timer_get_time();
    // Then the textured triangles, z-tested against the flat ones (timed
    // separately, inside se_scene_raster_textured).
    se_scene_raster_textured();
    int64_t const t_r1b = esp_timer_get_time();
    // Then the wireframe edges, z-tested against the depth the tris wrote.
    for (int i = 0; i < s_line_n; i++) {
        scene_raster_line(s_lines[i].v[0], s_lines[i].v[1], s_lines[i].packed);
    }
    int64_t const t_r2 = esp_timer_get_time();
    s_stat_tri_us  = t_r1 - t_r0;
    s_stat_line_us = t_r2 - t_r1b;
    // Points last, over everything (timed inside, like the textured pass).
    se_scene_raster_points();
}

// =====================================================================
//  Built-in renderer #2 -- SE_RENDER_BANDED (the z-buffer, one band at a time)
// ---------------------------------------------------------------------
//  The same passes as the z-buffer renderer, run once per vertical band
//  of SE_SCENE_BAND_W logical columns. A logical column is one raw row of
//  the rotated framebuffer, so a band is one contiguous block of it: it
//  is copied into a colour buffer in internal SRAM (whatever the frame
//  already holds there, e.g. the backdrop), drawn against a plain 16-bit
//  depth buffer in internal SRAM, and copied back. The per-pixel depth
//  test and the pixel writes then never touch PSRAM; PSRAM sees one
//  sequential read and one sequential write per band instead. Bands no
//  primitive touches are skipped outright.
//
//  The image is the z-buffer renderer's, pixel for pixel: same passes in
//  the same order, same depth encoding, and an empty depth cell reads 0
//  in both.
//
//  prepare() works out which bands each primitive spans, so a band only
//  sets up the primitives it contains. A primitive spanning several bands
//  is set up once per band: that is the price. rasterize() only reads
//  those spans and the lists, and draws each band through its own raster
//  target (s_rt), which is what lets a second core take bands later: it
//  needs its own band buffers and target, nothing else.
// =====================================================================

#define BAND_W     SE_SCENE_BAND_W
#define BAND_MAX_N ((DISPLAY_LOG_W + BAND_W - 1) / BAND_W)
_Static_assert(BAND_W >= 4 && BAND_MAX_N <= 255, "SE_SCENE_BAND_W: band numbers are 8-bit");

// The first and last band a primitive touches; b0 > b1 when none.
typedef struct {
    uint8_t b0, b1;
} band_span_t;

#define BAND_PIXELS (BAND_W * DISPLAY_RAW_STRIDE)   // a band at full resolution (the larger)

static uint16_t*    s_band_fb     = NULL;
static uint16_t*    s_band_dz     = NULL;
static band_span_t* s_band_tri    = NULL;   // SE_SCENE_TRI_CAP
static band_span_t* s_band_ttri   = NULL;   // SE_SCENE_TEXTURED_TRI_CAP
static band_span_t* s_band_line   = NULL;   // SE_SCENE_LINE_CAP
static band_span_t* s_band_pt     = NULL;   // SE_SCENE_POINT_CAP
static bool         s_band_used[BAND_MAX_N];
static bool         s_band_ok     = false;  // spans prepared, buffers there
static uint16_t     s_band_frame  = 0;      // ... for this frame (s_frame)
static bool         s_band_failed = false;  // allocation failed; logged once, not retried

// The band buffers must be internal SRAM, or banding buys nothing. The
// spans go to PSRAM: each band reads them once, in order, which the cache
// serves well, and internal SRAM is what the band buffers need.
static void* band_alloc_span(size_t n) {
    return heap_caps_malloc(n * sizeof(band_span_t), MALLOC_CAP_SPIRAM);
}

static bool band_alloc(void) {
    if (s_band_fb != NULL) return true;
    if (s_band_failed) return false;
    size_t const bytes = (size_t)BAND_PIXELS * sizeof(uint16_t);
    uint16_t*    fb    = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL);
    uint16_t*    dz    = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL);
    s_band_tri         = band_alloc_span(SE_SCENE_TRI_CAP);
    s_band_ttri        = band_alloc_span(SE_SCENE_TEXTURED_TRI_CAP);
    s_band_line        = band_alloc_span(SE_SCENE_LINE_CAP);
    s_band_pt          = band_alloc_span(SE_SCENE_POINT_CAP);
    if (fb == NULL || dz == NULL || !s_band_tri || !s_band_ttri || !s_band_line || !s_band_pt) {
        ESP_LOGE(TAG, "banded renderer: no room for its buffers (2 x %uKB internal, largest free %uKB) -- using the z-buffer",
                 (unsigned)(bytes / 1024), (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        heap_caps_free(fb);
        heap_caps_free(dz);
        heap_caps_free(s_band_tri);
        heap_caps_free(s_band_ttri);
        heap_caps_free(s_band_line);
        heap_caps_free(s_band_pt);
        s_band_tri = s_band_ttri = s_band_line = s_band_pt = NULL;
        s_band_failed = true;
        return false;
    }
    s_band_fb = fb;
    s_band_dz = dz;
    ESP_LOGI(TAG, "banded renderer: %d-column bands, 2 x %uKB internal", BAND_W, (unsigned)(bytes / 1024));
    return true;
}

// The bands covering screen columns [xmin, xmax], clipped to the frame's
// viewport. Conservative: a band a primitive only grazes is drawn and
// finds nothing, which costs time but never a pixel.
static band_span_t band_span(float xmin, float xmax) {
    band_span_t const none = {1, 0};
    if (!(xmax >= (float)s_vp_x0) || !(xmin <= (float)s_vp_x1)) return none;   // also catches NaN
    int c0 = xmin <= (float)s_vp_x0 ? s_vp_x0 : floor_i(xmin);
    int c1 = xmax >= (float)s_vp_x1 ? s_vp_x1 : ceil_i(xmax);
    if (c0 < s_vp_x0) c0 = s_vp_x0;
    if (c1 > s_vp_x1) c1 = s_vp_x1;
    if (c0 > c1) return none;
    band_span_t const sp = {(uint8_t)(c0 / BAND_W), (uint8_t)(c1 / BAND_W)};
    for (int b = sp.b0; b <= sp.b1; b++) s_band_used[b] = true;
    return sp;
}

static inline float min3f(float a, float b, float c) {
    float const m = a < b ? a : b;
    return m < c ? m : c;
}

static inline float max3f(float a, float b, float c) {
    float const m = a > b ? a : b;
    return m > c ? m : c;
}

static void banded_prepare(void* user) {
    (void)user;
    s_band_ok    = band_alloc();
    s_band_frame = s_frame;
    if (!s_band_ok) return;
    memset(s_band_used, 0, sizeof(s_band_used));
    for (int i = 0; i < s_tri_n; i++) {
        scene_vtx_t const* v = s_tris[i].v;
        s_band_tri[i]        = band_span(min3f(v[0].sx, v[1].sx, v[2].sx), max3f(v[0].sx, v[1].sx, v[2].sx));
    }
    for (int i = 0; i < s_ttri_n; i++) {
        se_tvtx_t const* v = s_ttris[i].v;
        s_band_ttri[i]     = band_span(min3f(v[0].sx, v[1].sx, v[2].sx), max3f(v[0].sx, v[1].sx, v[2].sx));
    }
    // Edges and points are placed by rounding, as their rasterizers do.
    for (int i = 0; i < s_line_n; i++) {
        float const a = (float)lroundf(s_lines[i].v[0].sx), b = (float)lroundf(s_lines[i].v[1].sx);
        s_band_line[i] = band_span(a < b ? a : b, a < b ? b : a);
    }
    for (int i = 0; i < s_pt_n; i++) {
        float const x = (float)lroundf(s_pts[i].v.sx);
        s_band_pt[i]  = band_span(x, x);
    }
}

typedef struct {
    int64_t tri_us, ttri_us, line_us, pt_us;
} band_times_t;

static inline bool band_has(band_span_t sp, int b) {
    return b >= sp.b0 && b <= sp.b1;
}

// Draw band `b`, columns [x0, x1] of the frame target `frame`, through
// the band buffers. Uses nothing a second core could not have its own of:
// the band buffers, s_rt, and the read-only lists and spans.
static void band_draw(int b, int x0, int x1, raster_target_t const* frame, band_times_t* t) {
    int const       off   = x0 * s_raw_stride;   // raw rows x0..x1 are contiguous
    size_t const    bytes = (size_t)(x1 - x0 + 1) * s_raw_stride * sizeof(uint16_t);
    uint16_t* const src   = frame->fb + off;

    // The band is written back whole, so what the frame holds there comes
    // in first. Write back and drop its cache lines before reading: the
    // backdrop may have been written by DMA (the PPA) behind the cache,
    // and pixels the CPU drew may still be dirty in it.
    esp_cache_msync(src, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                                    ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    memcpy(s_band_fb, src, bytes);
    memset(s_band_dz, 0, bytes);

    raster_target_t const acc = s_rt;   // counters so far
    s_rt = (raster_target_t){
        .fb = s_band_fb, .ds = frame->ds, .dz = s_band_dz, .dz_on = true, .off = off,
        .vp_x0 = x0, .vp_y0 = frame->vp_y0, .vp_x1 = x1, .vp_y1 = frame->vp_y1,
        .tri_px = acc.tri_px, .tri_sp = acc.tri_sp, .ttri_px = acc.ttri_px, .ttri_sp = acc.ttri_sp,
    };

    // The z-buffer renderer's passes and order (zbuf_rasterize).
    int64_t const t0 = esp_timer_get_time();
    for (int i = 0; i < s_tri_n; i++) {
        if (band_has(s_band_tri[i], b))
            scene_raster_tri(s_tris[i].v[0], s_tris[i].v[1], s_tris[i].v[2], s_tris[i].packed);
    }
    int64_t const t1 = esp_timer_get_time();
    for (int i = 0; i < s_ttri_n; i++) {
        if (band_has(s_band_ttri[i], b)) scene_raster_ttri(&s_ttris[i]);
    }
    int64_t const t2 = esp_timer_get_time();
    for (int i = 0; i < s_line_n; i++) {
        if (band_has(s_band_line[i], b)) scene_raster_line(s_lines[i].v[0], s_lines[i].v[1], s_lines[i].packed);
    }
    int64_t const t3 = esp_timer_get_time();
    for (int i = 0; i < s_pt_n; i++) {
        if (band_has(s_band_pt[i], b)) scene_raster_point(&s_pts[i]);
    }
    int64_t const t4 = esp_timer_get_time();
    t->tri_us  += t1 - t0;
    t->ttri_us += t2 - t1;
    t->line_us += t3 - t2;
    t->pt_us   += t4 - t3;

    memcpy(src, s_band_fb, bytes);
}

static void banded_rasterize(void* user) {
    // Spans from another frame (prepared with another renderer) would
    // put primitives in the wrong bands.
    if (!s_band_ok || s_band_frame != s_frame) {
        zbuf_rasterize(user);
        return;
    }
    raster_target_t const frame = s_rt;
    band_times_t          t     = {0};
    int const             nb    = (DISPLAY_LOG_W / s_div + BAND_W - 1) / BAND_W;
    for (int b = 0; b < nb; b++) {
        if (!s_band_used[b]) continue;
        int x0 = b * BAND_W, x1 = x0 + BAND_W - 1;
        if (x0 < frame.vp_x0) x0 = frame.vp_x0;
        if (x1 > frame.vp_x1) x1 = frame.vp_x1;
        if (x0 > x1) continue;
        band_draw(b, x0, x1, &frame, &t);
    }
    // Back to the frame target, keeping the counters the bands added up.
    raster_target_t const acc = s_rt;
    s_rt         = frame;
    s_rt.tri_px  = acc.tri_px;
    s_rt.tri_sp  = acc.tri_sp;
    s_rt.ttri_px = acc.ttri_px;
    s_rt.ttri_sp = acc.ttri_sp;
    s_stat_tri_us  = t.tri_us;
    s_stat_ttri_us = t.ttri_us;
    s_stat_line_us = t.line_us;
    s_stat_pt_us   = t.pt_us;
}

// =====================================================================
//  Renderer table + dispatch
// =====================================================================

static se_renderer_t s_renderers[SE_RENDER_MAX] = {
    [SE_RENDER_ZBUFFER] = { "zbuffer", zbuf_prepare,    zbuf_rasterize,    NULL },
    [SE_RENDER_BANDED]  = { "banded",  banded_prepare,  banded_rasterize,  NULL },
};
static int s_renderer_n = SE_RENDER_BUILTIN_COUNT;

se_render_mode_t se_renderer_register(se_renderer_t const* r) {
    if (!r || !r->prepare || !r->rasterize || s_renderer_n >= SE_RENDER_MAX) {
        ESP_LOGE(TAG, "renderer registration rejected (table %d/%d)",
                 s_renderer_n, SE_RENDER_MAX);
        return SE_RENDER_DEFAULT;
    }
    se_render_mode_t const h = (se_render_mode_t)s_renderer_n++;
    s_renderers[h] = *r;
    ESP_LOGI(TAG, "renderer '%s' registered as %d", r->name ? r->name : "?", (int)h);
    return h;
}

char const* se_renderer_name(se_render_mode_t mode) {
    if ((int)mode < 0 || (int)mode >= s_renderer_n) return "?";
    char const* const n = s_renderers[mode].name;
    return n ? n : "?";
}

// Resolve a mode to a usable renderer, falling back to the default rather
// than dereferencing a bogus handle.
static se_renderer_t const* renderer_for(se_render_mode_t mode) {
    if ((int)mode < 0 || (int)mode >= s_renderer_n || !s_renderers[mode].rasterize) {
        return &s_renderers[SE_RENDER_DEFAULT];
    }
    return &s_renderers[mode];
}

se_geometry_t se_scene_geometry(void) {
    return (se_geometry_t){
        .tris        = s_tris,
        .tri_n       = s_tri_n,
        .segs        = s_lines,
        .seg_n       = s_line_n,
        .fb          = s_fb,
        .depth       = s_dz_on ? NULL : s_ds,
        .depth16     = s_dz_on ? s_dz : NULL,
        .frame       = s_frame,
        .depth_scale = SCENE_DEPTH_SCALE,
        .ttris       = s_ttris,
        .ttri_n      = s_ttri_n,
        .pts         = s_pts,
        .pt_n        = s_pt_n,
        .scale       = s_div,
    };
}

void scene_prepare(se_render_mode_t mode) {
    // Geometry-only passes -- they touch the deferred lists, never the
    // framebuffer, so this half is safe to run concurrently with a hardware
    // blit writing the framebuffer (e.g. the PPA backdrop). See the header.
    scene_cull_pass();    // frustum cull (opt-in; no-op when disabled)
    scene_order_pass();   // front-to-back order (opt-in; no-op when disabled)
    se_renderer_t const* const r = renderer_for(mode);
    r->prepare(r->user);
}

void scene_rasterize(se_render_mode_t mode) {
    if (!s_tris || !s_lines || !s_ds || !s_fb) return;

    // Diagnostics: counts (post-cull) + per-phase wallclock, so a profiler
    // can see how the rasterize splits between filled geometry and the
    // wireframe edges -- and so a custom renderer can be compared with it.
    s_stat_tri_n  = s_tri_n;
    s_stat_line_n = s_line_n;
    s_stat_ttri_n  = s_ttri_n;
    s_stat_ttri_us = 0;   // stays 0 if a custom renderer skips the textured pass
    // The whole frame is the target, unless a renderer draws into others
    // (the banded one); either way the counters are read back from it.
    s_rt = (raster_target_t){
        .fb = s_fb, .ds = s_ds, .dz = s_dz, .dz_on = s_dz_on, .off = 0,
        .vp_x0 = s_vp_x0, .vp_y0 = s_vp_y0, .vp_x1 = s_vp_x1, .vp_y1 = s_vp_y1,
    };
    s_stat_tri_drop  = 0;
    s_stat_ttri_drop = 0;
    s_stat_pt_n    = s_pt_n;
    s_stat_pt_us   = 0;   // likewise for the point pass

    se_renderer_t const* const r = renderer_for(mode);
    r->rasterize(r->user);
    s_stat_tri_px  = s_rt.tri_px;
    s_stat_ttri_px = s_rt.ttri_px;
    s_stat_tri_sp  = s_rt.tri_sp;
    s_stat_ttri_sp = s_rt.ttri_sp;

    s_tri_n  = 0;
    s_line_n = 0;
    s_ttri_n = 0;
    s_pt_n   = 0;
}

void scene_raster_stats(int* tri_n, int* line_n, int64_t* tri_us, int64_t* line_us) {
    if (tri_n)   *tri_n   = s_stat_tri_n;
    if (line_n)  *line_n  = s_stat_line_n;
    if (tri_us)  *tri_us  = s_stat_tri_us;
    if (line_us) *line_us = s_stat_line_us;
}

void scene_textured_stats(int* ttri_n, int64_t* ttri_us) {
    if (ttri_n)  *ttri_n  = s_stat_ttri_n;
    if (ttri_us) *ttri_us = s_stat_ttri_us;
}

void scene_drop_stats(int* tris, int* ttris) {
    if (tris) *tris = s_stat_tri_drop;
    if (ttris) *ttris = s_stat_ttri_drop;
}

void scene_fill_stats(int64_t* tri_px, int64_t* ttri_px, int64_t* tri_spans, int64_t* ttri_spans) {
    if (tri_px)     *tri_px     = s_stat_tri_px;
    if (ttri_px)    *ttri_px    = s_stat_ttri_px;
    if (tri_spans)  *tri_spans  = s_stat_tri_sp;
    if (ttri_spans) *ttri_spans = s_stat_ttri_sp;
}

void scene_point_stats(int* pt_n, int64_t* pt_us) {
    if (pt_n)  *pt_n  = s_stat_pt_n;
    if (pt_us) *pt_us = s_stat_pt_us;
}

void scene_render(se_render_mode_t mode) {
    scene_prepare(mode);     // cull + order + renderer prepare (no framebuffer)
    scene_rasterize(mode);   // paint the prepared geometry
}

void scene_flush(void) {
    scene_render(SE_RENDER_ZBUFFER);
}
