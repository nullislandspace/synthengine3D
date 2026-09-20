#include "se_scene.h"

#include <math.h>
#include <stdlib.h>         // qsort (depth-order pass)
#include <string.h>

#include "se_config.h"      // DISPLAY_* + RENDER_* projection constants
#include "se_direct565.h"   // direct_565_logical_index, direct_565_pack
#include "esp_heap_caps.h"
#include "esp_log.h"
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
    // Advance the frame tag; skip 0 so a zero-initialised stamp cell
    // is never mistaken for "written this frame".
    s_frame++;
    if (s_frame == 0) s_frame = 1;
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
static inline void scene_vrun(int lx, int y_top, int y_bot,
                              float As, float Bs, float Cs, uint16_t packed) {
    if (lx < s_vp_x0 || lx > s_vp_x1) return;
    if (y_top < s_vp_y0) y_top = s_vp_y0;
    if (y_bot > s_vp_y1) y_bot = s_vp_y1;
    if (y_top > y_bot) return;

    uint16_t const frame = s_frame;
    uint32_t const fhi   = (uint32_t)frame << 16;   // stamp pre-shifted for the store
    int const idx = scene_index(lx, y_top);
    uint16_t* fp  = s_fb + idx;
    uint32_t* dp  = s_ds + idx;
    float     d   = As * (float)lx + Bs * (float)y_top + Cs;
    int       cnt = y_bot - y_top + 1;
    s_stat_tri_px += cnt;
    s_stat_tri_sp++;
    while (cnt-- > 0) {
        int di = (int)d;
        if (di < 0) di = 0;                      // sub-pixel edge overshoot guard
        uint32_t const cell   = *dp;
        // Stale stamp (≠ this frame) reads as depth 0 (infinitely far).
        uint16_t const stored = ((uint16_t)(cell >> 16) == frame) ? (uint16_t)cell : 0;
        if ((uint16_t)di > stored) {
            *dp = fhi | (uint16_t)di;
            *fp = packed;
        }
        fp--; dp--;
        d += Bs;
    }
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

    if (x2 < (float)s_vp_x0 || x0 > (float)s_vp_x1) return;
    if (x2 - x0 < 1e-6f) return;

    float const dydx_02 = (y2 - y0) / (x2 - x0);
    float const dydx_01 = (x1 > x0) ? (y1 - y0) / (x1 - x0) : 0.0f;
    float const dydx_12 = (x2 > x1) ? (y2 - y1) / (x2 - x1) : 0.0f;

    int ix_start =     ceil_i(x0);
    int ix_split =     ceil_i(x1);
    int ix_endex = 1 + floor_i(x2);
    if (ix_start < s_vp_x0)      ix_start = s_vp_x0;
    if (ix_endex > s_vp_x1 + 1)  ix_endex = s_vp_x1 + 1;
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
    uint32_t        shade;        // 0..32
} ttri_setup_t;

static inline void scene_vrun_tex(int lx, int y_top, int y_bot, ttri_setup_t const* st) {
    if (lx < s_vp_x0 || lx > s_vp_x1) return;
    if (y_top < s_vp_y0) y_top = s_vp_y0;
    if (y_bot > s_vp_y1) y_bot = s_vp_y1;
    if (y_top > y_bot) return;

    uint16_t const  frame = s_frame;
    uint32_t const  fhi   = (uint32_t)frame << 16;
    int const       idx   = scene_index(lx, y_top);
    uint16_t*       fp    = s_fb + idx;
    uint32_t*       dp    = s_ds + idx;
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
    s_stat_ttri_px += cnt;
    s_stat_ttri_sp++;
    while (cnt-- > 0) {
        int di = (int)d;
        if (di < 0) di = 0;
        uint32_t const cell   = *dp;
        uint16_t const stored = ((uint16_t)(cell >> 16) == frame) ? (uint16_t)cell : 0;
        if ((uint16_t)di > stored) {
            *dp = fhi | (uint16_t)di;
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
        dp--;
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
static inline void scene_vrun_tex_cutout(int lx, int y_top, int y_bot, ttri_setup_t const* st) {
    if (lx < s_vp_x0 || lx > s_vp_x1) return;
    if (y_top < s_vp_y0) y_top = s_vp_y0;
    if (y_bot > s_vp_y1) y_bot = s_vp_y1;
    if (y_top > y_bot) return;

    uint16_t const  frame = s_frame;
    uint32_t const  fhi   = (uint32_t)frame << 16;
    int const       idx   = scene_index(lx, y_top);
    uint16_t*       fp    = s_fb + idx;
    uint32_t*       dp    = s_ds + idx;
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
    s_stat_ttri_px += cnt;
    s_stat_ttri_sp++;
    while (cnt-- > 0) {
        int di = (int)d;
        if (di < 0) di = 0;
        uint32_t const cell   = *dp;
        uint16_t const stored = ((uint16_t)(cell >> 16) == frame) ? (uint16_t)cell : 0;
        if ((uint16_t)di > stored) {
            // di >= 1 here, so d >= 1: the divide is safe.
            float const    inv = 1.0f / d;
            uint32_t const tu  = (uint32_t)(int)(us * inv) & wm;
            uint32_t const tv  = (uint32_t)(int)(vs * inv) & hm;
            uint32_t const t   = tx[(tv << wl) | tu];
            if (t != SE_TEXEL_CUTOUT) {
                *dp        = fhi | (uint16_t)di;
                uint32_t x = (t | (t << 16)) & 0x07E0F81Fu;
                x          = ((x * sh) >> 5) & 0x07E0F81Fu;
                uint16_t px = (uint16_t)(x | (x >> 16));
                if (rev) px = (uint16_t)((px >> 8) | (px << 8));
                *fp = px;
            }
        }
        fp--;
        dp--;
        d  += Bd;
        us += Bu;
        vs += Bv;
    }
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
    st.shade  = t->shade;
    bool const cutout = t->tex->cutout;

    // Column scan, as in scene_raster_tri.
    float x0 = a.sx, y0 = a.sy, x1 = b.sx, y1 = b.sy, x2 = c.sx, y2 = c.sy;
    float tx, ty;
    if (x1 < x0) { tx=x0; ty=y0; x0=x1; y0=y1; x1=tx; y1=ty; }
    if (x2 < x0) { tx=x0; ty=y0; x0=x2; y0=y2; x2=tx; y2=ty; }
    if (x2 < x1) { tx=x1; ty=y1; x1=x2; y1=y2; x2=tx; y2=ty; }

    if (x2 < (float)s_vp_x0 || x0 > (float)s_vp_x1) return;
    if (x2 - x0 < 1e-6f) return;

    float const dydx_02 = (y2 - y0) / (x2 - x0);
    float const dydx_01 = (x1 > x0) ? (y1 - y0) / (x1 - x0) : 0.0f;
    float const dydx_12 = (x2 > x1) ? (y2 - y1) / (x2 - x1) : 0.0f;

    int ix_start =     ceil_i(x0);
    int ix_split =     ceil_i(x1);
    int ix_endex = 1 + floor_i(x2);
    if (ix_start < s_vp_x0)      ix_start = s_vp_x0;
    if (ix_endex > s_vp_x1 + 1)  ix_endex = s_vp_x1 + 1;
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

    int const idx = scene_index(x0, y0);
    uint16_t* fp  = s_fb + idx;
    uint32_t* dp  = s_ds + idx;
    int       lx  = x0;
    int       ly  = y0;

    while (1) {
        if (lx >= s_vp_x0 && lx <= s_vp_x1 && ly >= s_vp_y0 && ly <= s_vp_y1) {
            int di = (int)d;
            if (di < 0) di = 0;
            uint32_t const cell   = *dp;
            uint16_t const stored = ((uint16_t)(cell >> 16) == frame) ? (uint16_t)cell : 0;
            if ((uint16_t)di >= stored) *fp = packed;   // edges test depth but never write it
        }
        if (lx == x1 && ly == y1) break;
        int const e2 = 2 * err;
        if (e2 > -dy) { err -= dy; lx += sx; fp += ptr_dx; dp += ptr_dx; }
        if (e2 <  dx) { err += dx; ly += sy; fp += ptr_dy; dp += ptr_dy; }
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
    if (s_tri_n >= SCENE_TRI_CAP) return;   // overflow: drop extra tris
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
    if (behind == 0 && s_tri_n >= SCENE_TRI_CAP) return;   // overflow: skip the shading too

    // Lighting (se_light.h): shade the face once, from the WORLD-space
    // vertices -- the light lives in world space -- whatever clipping does
    // to it below: the pieces of a clipped face share its normal. Off by
    // default, and then this is a load and a branch. An emissive triangle
    // skips it and keeps the colour it was given.
    if (se_light_is_on && !(flags & SE_TRI_EMISSIVE)) {
        argb = se_light_shade_tri(argb, x0, y0, z0, x1, y1, z1, x2, y2, z2,
                                  s_camera.x, s_camera.y, s_camera.z);
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
    if (s_ttri_n >= SE_SCENE_TEXTURED_TRI_CAP) return;   // overflow: drop, as scene_tri does
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
    if (behind == 0 && s_ttri_n >= SE_SCENE_TEXTURED_TRI_CAP) return;

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
void se_scene_raster_points(void) {
    if (s_pts == NULL || s_fb == NULL || s_ds == NULL) return;
    int64_t const  t0    = esp_timer_get_time();
    uint16_t const frame = s_frame;
    for (int i = 0; i < s_pt_n; i++) {
        se_pt_t const* pt = &s_pts[i];
        int const      x  = (int)lroundf(pt->v.sx), y = (int)lroundf(pt->v.sy);
        if (x < s_vp_x0 || x > s_vp_x1 || y < s_vp_y0 || y > s_vp_y1) continue;
        int const      idx    = scene_index(x, y);
        uint32_t const cell   = s_ds[idx];
        uint16_t const stored = ((uint16_t)(cell >> 16) == frame) ? (uint16_t)cell : 0;
        int            di     = (int)(pt->v.w * SCENE_DEPTH_SCALE);
        if (di < 0) di = 0;
        if ((uint16_t)di >= stored) s_fb[idx] = pt->packed;
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

static void scene_order_pass(void) {
    if (!s_opts.depth_order) return;
    if (s_tri_n > 1) {
        qsort(s_tris, (size_t)s_tri_n, sizeof(scene_tri_t), tri_cmp_near_first);
    }
    // Sorted within their own list: the two lists rasterize one after
    // the other, so they cannot interleave. Early-z pays off more here
    // than for flat triangles, since an occluded textured pixel skips a
    // divide and a texel fetch as well as the framebuffer write.
    if (s_ttri_n > 1) {
        qsort(s_ttris, (size_t)s_ttri_n, sizeof(se_ttri_t), ttri_cmp_near_first);
    }
}

// =====================================================================
//  Built-in renderer #1 -- SE_RENDER_ZBUFFER (primitive-driven)
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
//  Built-in renderer #2 -- SE_RENDER_RAYCAST (pixel-driven)
// ---------------------------------------------------------------------
//  One primary ray per pixel, nearest hit wins, each pixel written at
//  most once. A ray that hits nothing writes NOTHING -- not the colour
//  and not the depth -- so the backdrop under it survives untouched,
//  exactly like the rasterizer, which only ever touches covered pixels.
//
//  Casting a ray per pixel against every triangle would be O(pixels x
//  tris) and hopeless here, so the triangles are first binned into
//  fixed screen tiles by their bounding box. A pixel then only tests the
//  handful of triangles binned to ITS tile, and wholly empty tiles --
//  usually most of the screen -- are skipped without touching a pixel.
//
//  Why this is a real alternative and not just a slower rasterizer: the
//  two have opposite cost profiles. The z-buffer pays per covered pixel
//  PER TRIANGLE (overdraw) in PSRAM traffic, which is this device's
//  scarce resource; the raycaster pays per pixel of a non-empty tile in
//  ALU work over a candidate list small enough to sit in cache, and
//  touches PSRAM once per visible pixel regardless of depth complexity.
//  Dense, heavily-overlapping scenes favour the raycaster; sparse ones
//  favour the rasterizer, because it never looks at a pixel no triangle
//  covers. Measure with scene_raster_stats() -- do not assume.
// =====================================================================

#define RC_TILE_SHIFT 4                                        // 16x16 px tiles
#define RC_TILE       (1 << RC_TILE_SHIFT)
#define RC_TILES_X    ((DISPLAY_LOG_W + RC_TILE - 1) / RC_TILE)
#define RC_TILES_Y    ((DISPLAY_LOG_H + RC_TILE - 1) / RC_TILE)
#define RC_TILES      (RC_TILES_X * RC_TILES_Y)

// Capacity of the (triangle, tile) bin pool. A triangle is binned once
// per tile its bbox touches, so this bounds total screen coverage, not
// triangle count. On overflow the frame falls back to the z-buffer
// renderer -- correct output, just not the renderer that was asked for.
#define RC_BIN_CAP    32768

// Candidates resolved in one pass over a tile's pixels. A tile holding
// more than this is processed in several passes; the depth test makes
// that safe (max-wins is order-independent), it just costs those pixels
// one extra write per extra pass. Sized so the setup table stays small
// enough to stay hot in cache.
#define RC_TILE_CANDS 192

// Per-triangle constants the pixel loop needs: three edge functions
// oriented so "inside" is E >= 0 for either winding, and the same depth
// plane the z-buffer rasterizer derives, in encoded uint16 units.
typedef struct {
    float e0a, e0b, e0c;
    float e1a, e1b, e1c;
    float e2a, e2b, e2c;
    float As, Bs, Cs;
    uint16_t packed;
} rc_setup_t;

static int32_t*   s_rc_off  = NULL;   // RC_TILES + 1 bin offsets
static uint16_t*  s_rc_bin  = NULL;   // RC_BIN_CAP triangle indices
static bool       s_rc_fail = false;  // this frame: fall back to z-buffer
static bool       s_rc_warned = false;     // alloc failure, logged once
static bool       s_rc_overflowed = false; // pool overflow, logged once
static rc_setup_t s_rc_setup[RC_TILE_CANDS];

// Lazily allocate the bin structures the first time the raycaster is
// selected, so a game that only ever uses the z-buffer pays nothing.
static bool rc_alloc(void) {
    if (s_rc_off && s_rc_bin) return true;
    if (!s_rc_off) {
        s_rc_off = heap_caps_malloc((RC_TILES + 1) * sizeof(int32_t), MALLOC_CAP_INTERNAL);
    }
    if (!s_rc_bin) {
        size_t const sz = (size_t)RC_BIN_CAP * sizeof(uint16_t);
        s_rc_bin = heap_caps_malloc(sz, MALLOC_CAP_INTERNAL);
        if (!s_rc_bin) s_rc_bin = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    }
    if (!s_rc_off || !s_rc_bin) {
        if (!s_rc_warned) {
            ESP_LOGE(TAG, "raycast bin alloc failed (off=%p bin=%p) -- using z-buffer",
                     s_rc_off, s_rc_bin);
            s_rc_warned = true;
        }
        return false;
    }
    ESP_LOGI(TAG, "raycast bins: %dx%d tiles, pool %u entries (%uKB)",
             RC_TILES_X, RC_TILES_Y, (unsigned)RC_BIN_CAP,
             (unsigned)((RC_BIN_CAP * sizeof(uint16_t)) / 1024));
    return true;
}

// Screen-space tile range a triangle's bounding box touches. Returns
// false if the bbox is entirely off-screen (it then occupies no bin).
static inline bool rc_tri_tiles(scene_tri_t const* t,
                                int* tx0, int* ty0, int* tx1, int* ty1) {
    float minx = t->v[0].sx, maxx = minx;
    float miny = t->v[0].sy, maxy = miny;
    for (int k = 1; k < 3; k++) {
        float const x = t->v[k].sx, y = t->v[k].sy;
        if (x < minx) minx = x; else if (x > maxx) maxx = x;
        if (y < miny) miny = y; else if (y > maxy) maxy = y;
    }
    int ix0 = floor_i(minx), ix1 = ceil_i(maxx);
    int iy0 = floor_i(miny), iy1 = ceil_i(maxy);
    if (ix0 < s_vp_x0) ix0 = s_vp_x0;
    if (iy0 < s_vp_y0) iy0 = s_vp_y0;
    if (ix1 > s_vp_x1) ix1 = s_vp_x1;
    if (iy1 > s_vp_y1) iy1 = s_vp_y1;
    if (ix0 > ix1 || iy0 > iy1) return false;   // wholly outside the viewport
    *tx0 = ix0 >> RC_TILE_SHIFT; *tx1 = ix1 >> RC_TILE_SHIFT;
    *ty0 = iy0 >> RC_TILE_SHIFT; *ty1 = iy1 >> RC_TILE_SHIFT;
    return true;
}

// Bin every triangle into the tiles its bbox covers, as a counting sort:
// count per tile, prefix-sum to starts, then fill using the starts as
// cursors. After the fill each cursor has advanced to its tile's END, and
// the previous tile's cursor is this tile's start -- so tile t occupies
// bin[ t ? off[t-1] : 0 .. off[t] ). No separate cursor array needed.
static bool rc_bin_build(void) {
    int32_t* const off = s_rc_off;
    memset(off, 0, (RC_TILES + 1) * sizeof(int32_t));

    int32_t total = 0;
    for (int i = 0; i < s_tri_n; i++) {
        int tx0, ty0, tx1, ty1;
        if (!rc_tri_tiles(&s_tris[i], &tx0, &ty0, &tx1, &ty1)) continue;
        for (int ty = ty0; ty <= ty1; ty++) {
            int const row = ty * RC_TILES_X;
            for (int tx = tx0; tx <= tx1; tx++) off[row + tx + 1]++;
        }
        total += (int32_t)(tx1 - tx0 + 1) * (int32_t)(ty1 - ty0 + 1);
        if (total > RC_BIN_CAP) return false;   // pool would overflow
    }
    for (int i = 1; i <= RC_TILES; i++) off[i] += off[i - 1];

    for (int i = 0; i < s_tri_n; i++) {
        int tx0, ty0, tx1, ty1;
        if (!rc_tri_tiles(&s_tris[i], &tx0, &ty0, &tx1, &ty1)) continue;
        for (int ty = ty0; ty <= ty1; ty++) {
            int const row = ty * RC_TILES_X;
            for (int tx = tx0; tx <= tx1; tx++) s_rc_bin[off[row + tx]++] = (uint16_t)i;
        }
    }
    return true;
}

// Per-triangle pixel-loop constants. The depth plane is derived exactly as
// in scene_raster_tri, so both renderers interpolate identical depths. The
// edge functions are negated for clockwise triangles (nz is twice the
// signed area) so the inside test is E >= 0 regardless of winding.
static bool rc_setup(scene_tri_t const* t, rc_setup_t* st) {
    se_vtx_t const a = t->v[0], b = t->v[1], c = t->v[2];
    float const ex1 = b.sx - a.sx, ey1 = b.sy - a.sy, ew1 = b.w - a.w;
    float const ex2 = c.sx - a.sx, ey2 = c.sy - a.sy, ew2 = c.w - a.w;
    float const nx  = ey1 * ew2 - ew1 * ey2;
    float const ny  = ew1 * ex2 - ex1 * ew2;
    float const nz  = ex1 * ey2 - ey1 * ex2;
    if (nz > -1e-6f && nz < 1e-6f) return false;   // degenerate
    float const inv_nz = 1.0f / nz;
    st->As = (-nx * inv_nz) * SCENE_DEPTH_SCALE;
    st->Bs = (-ny * inv_nz) * SCENE_DEPTH_SCALE;
    st->Cs = a.w * SCENE_DEPTH_SCALE - st->As * a.sx - st->Bs * a.sy;

    float const s = (nz < 0.0f) ? -1.0f : 1.0f;
    st->e0a = s * -(b.sy - a.sy); st->e0b = s * (b.sx - a.sx);
    st->e0c = s * ((b.sy - a.sy) * a.sx - (b.sx - a.sx) * a.sy);
    st->e1a = s * -(c.sy - b.sy); st->e1b = s * (c.sx - b.sx);
    st->e1c = s * ((c.sy - b.sy) * b.sx - (c.sx - b.sx) * b.sy);
    st->e2a = s * -(a.sy - c.sy); st->e2b = s * (a.sx - c.sx);
    st->e2c = s * ((a.sy - c.sy) * c.sx - (a.sx - c.sx) * c.sy);
    st->packed = t->packed;
    return true;
}

// Cast every pixel of one tile against `n` candidates. Sampling is at
// integer pixel coordinates, the same points the z-buffer rasterizer
// samples, so the two renderers agree except where a sample lands exactly
// on a shared edge and the tie breaks the other way.
//
// Iteration is x-outer / y-inner because a logical +1 y step is a -1 step
// in the framebuffer index (see direct_565_logical_index) -- so the inner
// loop walks memory contiguously, matching the rasterizer's column scan.
static void rc_cast_tile(int tx, int ty, rc_setup_t const* set, int n) {
    // A tile straddling the viewport edge is resolved only over the part
    // inside it; one wholly outside was never binned, so it never gets
    // here.
    int x0 = tx << RC_TILE_SHIFT;
    int y0 = ty << RC_TILE_SHIFT;
    int x1 = x0 + RC_TILE, y1 = y0 + RC_TILE;
    if (x0 < s_vp_x0) x0 = s_vp_x0;
    if (y0 < s_vp_y0) y0 = s_vp_y0;
    if (x1 > s_vp_x1 + 1) x1 = s_vp_x1 + 1;
    if (y1 > s_vp_y1 + 1) y1 = s_vp_y1 + 1;
    if (x0 >= x1 || y0 >= y1) return;

    uint16_t const frame = s_frame;
    int      const base  = scene_index(x0, y0);

    for (int x = x0; x < x1; x++) {
        float const fx  = (float)x;
        int         idx = base + (x - x0) * DISPLAY_RAW_STRIDE;
        for (int y = y0; y < y1; y++, idx--) {
            float const fy = (float)y;
            float    best  = 0.0f;
            uint16_t bestp = 0;
            bool     hit   = false;
            // Nearest hit along the ray. The early-out on the first
            // failing edge is what makes a fat candidate list cheap:
            // a triangle whose tile the pixel is outside of costs one
            // multiply-add and a compare.
            for (int k = 0; k < n; k++) {
                rc_setup_t const* const st = &set[k];
                if (st->e0a * fx + st->e0b * fy + st->e0c < 0.0f) continue;
                if (st->e1a * fx + st->e1b * fy + st->e1c < 0.0f) continue;
                if (st->e2a * fx + st->e2b * fy + st->e2c < 0.0f) continue;
                float const d = st->As * fx + st->Bs * fy + st->Cs;
                if (!hit || d > best) { best = d; bestp = st->packed; hit = true; }
            }
            if (!hit) continue;   // ray missed -> pixel (and its depth) untouched

            int di = (int)best;
            if (di < 0) di = 0;
            // Still depth-tested: a tile with more candidates than
            // RC_TILE_CANDS is resolved in several passes, and the later
            // passes must lose to nearer pixels the earlier ones wrote.
            uint32_t const cell   = s_ds[idx];
            uint16_t const stored = ((uint16_t)(cell >> 16) == frame) ? (uint16_t)cell : 0;
            if ((uint16_t)di >= stored) {
                s_fb[idx] = bestp;
                s_ds[idx] = ((uint32_t)frame << 16) | (uint32_t)di;
            }
        }
    }
}

static void raycast_prepare(void* user) {
    (void)user;
    // Falling back is not an error path the caller has to handle: the
    // frame still renders, just with the other renderer.
    if (!rc_alloc()) { s_rc_fail = true; return; }   // rc_alloc() logs once itself
    s_rc_fail = !rc_bin_build();
    if (s_rc_fail && !s_rc_overflowed) {
        ESP_LOGW(TAG, "raycast bin pool exhausted (%d tris) -- frame falls back to z-buffer",
                 s_tri_n);
        s_rc_overflowed = true;
    }
}

static void raycast_rasterize(void* user) {
    if (s_rc_fail) { zbuf_rasterize(user); return; }

    int64_t const t_r0 = esp_timer_get_time();
    int32_t const* const off = s_rc_off;
    for (int t = 0; t < RC_TILES; t++) {
        int32_t const lo = t ? off[t - 1] : 0;
        int32_t const hi = off[t];
        if (lo >= hi) continue;                   // empty tile: never touched
        int const tx = t % RC_TILES_X, ty = t / RC_TILES_X;
        // Chunked so a pathologically deep tile can't overrun the setup
        // table; the depth test makes multi-pass resolution correct.
        for (int32_t c = lo; c < hi; c += RC_TILE_CANDS) {
            int32_t const end = (hi - c > RC_TILE_CANDS) ? c + RC_TILE_CANDS : hi;
            int n = 0;
            for (int32_t k = c; k < end; k++) {
                if (rc_setup(&s_tris[s_rc_bin[k]], &s_rc_setup[n])) n++;
            }
            if (n) rc_cast_tile(tx, ty, s_rc_setup, n);
        }
    }
    int64_t const t_r1 = esp_timer_get_time();
    // Textured triangles are not cast: they go through the shared
    // textured rasterizer, depth-tested against what the ray hits wrote
    // (same depth encoding), so the two compose exactly as in the
    // z-buffer renderer.
    se_scene_raster_textured();
    int64_t const t_r1b = esp_timer_get_time();
    // Wireframe edges are an overlay in both renderers: same Bresenham
    // pass, z-tested against the depth the ray hits just wrote.
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
//  Renderer table + dispatch
// =====================================================================

static se_renderer_t s_renderers[SE_RENDER_MAX] = {
    [SE_RENDER_ZBUFFER] = { "zbuffer", zbuf_prepare,    zbuf_rasterize,    NULL },
    [SE_RENDER_RAYCAST] = { "raycast", raycast_prepare, raycast_rasterize, NULL },
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
    // The raycaster's tiles are sized for the full screen: at quarter
    // resolution the z-buffer renders instead.
    if (s_div != 1 && mode == SE_RENDER_RAYCAST) mode = SE_RENDER_ZBUFFER;
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
        .depth       = s_ds,
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
    // wireframe edges -- and so the two renderers can be compared directly.
    s_stat_tri_n  = s_tri_n;
    s_stat_line_n = s_line_n;
    s_stat_ttri_n  = s_ttri_n;
    s_stat_ttri_us = 0;   // stays 0 if a custom renderer skips the textured pass
    s_stat_tri_px  = 0;
    s_stat_ttri_px = 0;
    s_stat_tri_sp  = 0;
    s_stat_ttri_sp = 0;
    s_stat_pt_n    = s_pt_n;
    s_stat_pt_us   = 0;   // likewise for the point pass

    se_renderer_t const* const r = renderer_for(mode);
    r->rasterize(r->user);

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
