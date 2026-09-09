#include "se_scene.h"

#include <math.h>
#include <stdlib.h>         // qsort (depth-order pass)
#include <string.h>

#include "se_config.h"      // DISPLAY_* + RENDER_* projection constants
#include "se_direct565.h"   // direct_565_logical_index, direct_565_pack
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"      // scene_raster_stats per-phase timing

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
// projection. Near-clipped z is RENDER_NEAR_CLIP_Z (0.5), so 1/z
// peaks at 2.0; SCENE_DEPTH_SCALE maps that to 64000 — inside the
// uint16 range with headroom, so the rasterizer never has to clamp
// the high end. Larger encoded value = nearer.
#define SCENE_DEPTH_SCALE   32000.0f

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

// Deferred geometry caps. ~80 obstacles * (a few faces * 2 tris) + the
// ship + pickups stays well under the triangle cap; ~80 * ~14 edges fits
// the edge cap. Overflow silently drops extra geometry (see scene_tri /
// scene_line). At ~40 B/tri the triangle buffer is ~160 KB of PSRAM.
#define SCENE_TRI_CAP   4096
#define SCENE_LINE_CAP  4096

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

static uint16_t*    s_fb      = NULL;
static bool         s_rev     = false;
static uint16_t     s_frame   = 0;      // current frame tag (never 0 while live)

// Rasterize diagnostics — counts (post-cull) + per-phase wallclock of the
// most recent scene_rasterize(), reported by scene_raster_stats().
static int          s_stat_tri_n   = 0;
static int          s_stat_line_n  = 0;
static int64_t      s_stat_tri_us  = 0;
static int64_t      s_stat_line_us = 0;

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

void scene_begin(pax_buf_t* fb) {
    s_fb     = (uint16_t*)pax_buf_get_pixels(fb);
    s_rev    = fb->reverse_endianness;
    s_tri_n  = 0;
    s_line_n = 0;
    // Advance the frame tag; skip 0 so a zero-initialised stamp cell
    // is never mistaken for "written this frame".
    s_frame++;
    if (s_frame == 0) s_frame = 1;
}

// --- Projection ---------------------------------------------------------------

// Project a *camera-space* point (right, up, forward) to (screen x,
// screen y, 1/z). Forward-z is clamped to the near plane the same way
// the old per-object renderers clamped world-z, so the projection can't
// blow up and the visual result matches the pre-z-buffer pipeline. The
// world->camera rotate+translate is done once by camera_transform()
// before this, so a vertex shared between the near cull and the
// projection is transformed only once.
static inline void scene_project_cam(float cx, float cy, float cz, scene_vtx_t* out) {
    if (cz < RENDER_NEAR_CLIP_Z) cz = RENDER_NEAR_CLIP_Z;
    float const inv_z = 1.0f / cz;
    out->sx = RENDER_HALF_W    + RENDER_FOCAL_LEN * cx * inv_z;
    out->sy = RENDER_HORIZON_Y - RENDER_FOCAL_LEN * cy * inv_z;
    out->w  = inv_z;
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
    if (lx < 0 || lx >= DISPLAY_LOG_W) return;
    if (y_top < 0)              y_top = 0;
    if (y_bot >= DISPLAY_LOG_H) y_bot = DISPLAY_LOG_H - 1;
    if (y_top > y_bot) return;

    uint16_t const frame = s_frame;
    uint32_t const fhi   = (uint32_t)frame << 16;   // stamp pre-shifted for the store
    int const idx = direct_565_logical_index(lx, y_top);
    uint16_t* fp  = s_fb + idx;
    uint32_t* dp  = s_ds + idx;
    float     d   = As * (float)lx + Bs * (float)y_top + Cs;
    int       cnt = y_bot - y_top + 1;
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

    if (x2 <= 0.0f || x0 >= (float)DISPLAY_LOG_W) return;
    if (x2 - x0 < 1e-6f) return;

    float const dydx_02 = (y2 - y0) / (x2 - x0);
    float const dydx_01 = (x1 > x0) ? (y1 - y0) / (x1 - x0) : 0.0f;
    float const dydx_12 = (x2 > x1) ? (y2 - y1) / (x2 - x1) : 0.0f;

    int ix_start =     (int)ceilf(x0);
    int ix_split =     (int)ceilf(x1);
    int ix_endex = 1 + (int)floorf(x2);
    if (ix_start < 0)             ix_start = 0;
    if (ix_endex > DISPLAY_LOG_W) ix_endex = DISPLAY_LOG_W;
    if (ix_split < ix_start)      ix_split = ix_start;
    if (ix_split > ix_endex)      ix_split = ix_endex;

    for (int x = ix_start; x < ix_split; x++) {
        float const dx = (float)x - x0;
        float const ya = y0 + dydx_02 * dx;
        float const yb = y0 + dydx_01 * dx;
        float yt, yz;
        if (ya < yb) { yt = ya; yz = yb; } else { yt = yb; yz = ya; }
        scene_vrun(x, (int)ceilf(yt), (int)floorf(yz), As, Bs, Cs, packed);
    }
    for (int x = ix_split; x < ix_endex; x++) {
        float const dx02 = (float)x - x0;
        float const dx12 = (float)x - x1;
        float const ya   = y0 + dydx_02 * dx02;
        float const yb   = y1 + dydx_12 * dx12;
        float yt, yz;
        if (ya < yb) { yt = ya; yz = yb; } else { yt = yb; yz = ya; }
        scene_vrun(x, (int)ceilf(yt), (int)floorf(yz), As, Bs, Cs, packed);
    }
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
    int     const ptr_dx = (sx > 0) ? DISPLAY_RAW_STRIDE : -DISPLAY_RAW_STRIDE;
    int     const ptr_dy = (sy > 0) ? -1 : 1;

    int const idx = direct_565_logical_index(x0, y0);
    uint16_t* fp  = s_fb + idx;
    uint32_t* dp  = s_ds + idx;
    int       lx  = x0;
    int       ly  = y0;

    while (1) {
        if (lx >= 0 && lx < DISPLAY_LOG_W && ly >= 0 && ly < DISPLAY_LOG_H) {
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

// --- Public submit / flush ----------------------------------------------------

void scene_tri(float x0, float y0, float z0,
               float x1, float y1, float z1,
               float x2, float y2, float z2, uint32_t argb) {
    if (!s_tris) return;
    float c0x, c0y, c0z, c1x, c1y, c1z, c2x, c2y, c2z;
    camera_transform(x0, y0, z0, &c0x, &c0y, &c0z);
    camera_transform(x1, y1, z1, &c1x, &c1y, &c1z);
    camera_transform(x2, y2, z2, &c2x, &c2y, &c2z);
    // Whole-triangle near cull: drop it only if every vertex is behind
    // the near plane in CAMERA space (so it is correct under any camera
    // pose, not just the forward-looking default; otherwise the per-
    // vertex clamp in scene_project_cam keeps the projection bounded).
    // This is a projection guard, not the central frustum cull (that is
    // scene_cull_pass, opt-in via scene_set_options).
    if (c0z < RENDER_NEAR_CLIP_Z && c1z < RENDER_NEAR_CLIP_Z && c2z < RENDER_NEAR_CLIP_Z) {
        return;
    }
    if (s_tri_n >= SCENE_TRI_CAP) return;   // overflow: drop extra tris
    scene_tri_t* t = &s_tris[s_tri_n++];
    scene_project_cam(c0x, c0y, c0z, &t->v[0]);
    scene_project_cam(c1x, c1y, c1z, &t->v[1]);
    scene_project_cam(c2x, c2y, c2z, &t->v[2]);
    t->packed = direct_565_pack(argb, s_rev);
}

void scene_line(float x0, float y0, float z0,
                float x1, float y1, float z1, uint32_t argb) {
    if (!s_lines) return;
    float c0x, c0y, c0z, c1x, c1y, c1z;
    camera_transform(x0, y0, z0, &c0x, &c0y, &c0z);
    camera_transform(x1, y1, z1, &c1x, &c1y, &c1z);
    if (c0z < RENDER_NEAR_CLIP_Z && c1z < RENDER_NEAR_CLIP_Z) return;
    if (s_line_n >= SCENE_LINE_CAP) return;
    scene_seg_t* seg = &s_lines[s_line_n++];
    scene_project_cam(c0x, c0y, c0z, &seg->v[0]);
    scene_project_cam(c1x, c1y, c1z, &seg->v[1]);
    seg->packed = direct_565_pack(argb, s_rev);
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
    float const W = (float)DISPLAY_LOG_W, H = (float)DISPLAY_LOG_H;
    if (t->v[0].sx <  0.0f && t->v[1].sx <  0.0f && t->v[2].sx <  0.0f) return true;
    if (t->v[0].sx >= W    && t->v[1].sx >= W    && t->v[2].sx >= W)    return true;
    if (t->v[0].sy <  0.0f && t->v[1].sy <  0.0f && t->v[2].sy <  0.0f) return true;
    if (t->v[0].sy >= H    && t->v[1].sy >= H    && t->v[2].sy >= H)    return true;
    return false;
}

static inline bool seg_offscreen(scene_seg_t const* s) {
    float const W = (float)DISPLAY_LOG_W, H = (float)DISPLAY_LOG_H;
    if (s->v[0].sx <  0.0f && s->v[1].sx <  0.0f) return true;
    if (s->v[0].sx >= W    && s->v[1].sx >= W)    return true;
    if (s->v[0].sy <  0.0f && s->v[1].sy <  0.0f) return true;
    if (s->v[0].sy >= H    && s->v[1].sy >= H)    return true;
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
static void scene_order_pass(void) {
    if (!s_opts.depth_order) return;
    if (s_tri_n > 1) {
        qsort(s_tris, (size_t)s_tri_n, sizeof(scene_tri_t), tri_cmp_near_first);
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
    // Then the wireframe edges, z-tested against the depth the tris wrote.
    for (int i = 0; i < s_line_n; i++) {
        scene_raster_line(s_lines[i].v[0], s_lines[i].v[1], s_lines[i].packed);
    }
    int64_t const t_r2 = esp_timer_get_time();
    s_stat_tri_us  = t_r1 - t_r0;
    s_stat_line_us = t_r2 - t_r1;
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
    int ix0 = (int)floorf(minx), ix1 = (int)ceilf(maxx);
    int iy0 = (int)floorf(miny), iy1 = (int)ceilf(maxy);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > DISPLAY_LOG_W - 1) ix1 = DISPLAY_LOG_W - 1;
    if (iy1 > DISPLAY_LOG_H - 1) iy1 = DISPLAY_LOG_H - 1;
    if (ix0 > ix1 || iy0 > iy1) return false;   // wholly off-screen
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
    int const x0 = tx << RC_TILE_SHIFT;
    int const y0 = ty << RC_TILE_SHIFT;
    int x1 = x0 + RC_TILE, y1 = y0 + RC_TILE;
    if (x1 > DISPLAY_LOG_W) x1 = DISPLAY_LOG_W;
    if (y1 > DISPLAY_LOG_H) y1 = DISPLAY_LOG_H;

    uint16_t const frame = s_frame;
    int      const base  = direct_565_logical_index(x0, y0);

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
    // Wireframe edges are an overlay in both renderers: same Bresenham
    // pass, z-tested against the depth the ray hits just wrote.
    for (int i = 0; i < s_line_n; i++) {
        scene_raster_line(s_lines[i].v[0], s_lines[i].v[1], s_lines[i].packed);
    }
    int64_t const t_r2 = esp_timer_get_time();
    s_stat_tri_us  = t_r1 - t_r0;
    s_stat_line_us = t_r2 - t_r1;
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

    se_renderer_t const* const r = renderer_for(mode);
    r->rasterize(r->user);

    s_tri_n  = 0;
    s_line_n = 0;
}

void scene_raster_stats(int* tri_n, int* line_n, int64_t* tri_us, int64_t* line_us) {
    if (tri_n)   *tri_n   = s_stat_tri_n;
    if (line_n)  *line_n  = s_stat_line_n;
    if (tri_us)  *tri_us  = s_stat_tri_us;
    if (line_us) *line_us = s_stat_line_us;
}

void scene_render(se_render_mode_t mode) {
    scene_prepare(mode);     // cull + order + renderer prepare (no framebuffer)
    scene_rasterize(mode);   // paint the prepared geometry
}

void scene_flush(void) {
    scene_render(SE_RENDER_ZBUFFER);
}
