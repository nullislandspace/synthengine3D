#include "se_scene.h"

#include <math.h>
#include <stdlib.h>         // qsort (depth-order pass)
#include <string.h>

#include "se_config.h"      // DISPLAY_* + RENDER_* projection constants
#include "se_direct565.h"   // direct_565_logical_index, direct_565_pack
#include "esp_heap_caps.h"
#include "esp_log.h"

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
// The depth buffer is never cleared. Instead a parallel 8-bit stamp
// plane records, per pixel, the frame number that last wrote a depth
// there. A depth value counts only when its stamp equals the current
// frame; a stale stamp reads as "infinitely far". So every frame
// starts with a logically-empty depth buffer for the cost of one
// counter increment — no 768 KB memset — and depth traffic happens
// only on pixels the 3D scene actually draws, not the whole screen.
//
// The stamp is 8-bit, so it wraps every 256 frames; a pixel that an
// obstacle covered, then went exactly a 256-frame multiple without
// being touched, then was covered again, could mis-resolve for one
// pixel for one frame. That is invisible in practice. Frame 0 is
// skipped on wrap so an untouched (zero-initialised) stamp cell never
// matches a live frame.

#define SCENE_PIXELS  (DISPLAY_LOG_W * DISPLAY_RAW_STRIDE)

// Deferred geometry caps. ~80 obstacles * (a few faces * 2 tris) + the
// ship + pickups stays well under the triangle cap; ~80 * ~14 edges fits
// the edge cap. Overflow silently drops extra geometry (see scene_tri /
// scene_line). At ~40 B/tri the triangle buffer is ~160 KB of PSRAM.
#define SCENE_TRI_CAP   4096
#define SCENE_LINE_CAP  4096

typedef struct {
    float sx, sy, w;   // projected screen x/y + 1/z depth
} scene_vtx_t;

typedef struct {
    scene_vtx_t v[3];
    uint16_t    packed;
} scene_tri_t;

typedef struct {
    scene_vtx_t v[2];
    uint16_t    packed;
} scene_seg_t;

static uint16_t*    s_depth   = NULL;   // encoded 1/z, indexed like the fb
static uint8_t*     s_stamp   = NULL;   // frame tag per pixel
static scene_tri_t* s_tris    = NULL;   // accumulated triangles (this frame)
static int          s_tri_n   = 0;
static scene_seg_t* s_lines   = NULL;   // accumulated wireframe edges
static int          s_line_n  = 0;

static uint16_t*    s_fb      = NULL;
static bool         s_rev     = false;
static uint8_t      s_frame   = 0;      // current frame tag (never 0 while live)

// Optional render passes (frustum cull / depth order). Both default OFF
// so scene_render() is behaviour- and byte-identical to the no-op cut
// until a game opts in. See scene_set_options().
static se_scene_options_t s_opts = { false, false };

void scene_init(void) {
    s_depth = heap_caps_malloc(SCENE_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_stamp = heap_caps_malloc(SCENE_PIXELS * sizeof(uint8_t),  MALLOC_CAP_SPIRAM);
    s_tris  = heap_caps_malloc(SCENE_TRI_CAP  * sizeof(scene_tri_t), MALLOC_CAP_SPIRAM);
    s_lines = heap_caps_malloc(SCENE_LINE_CAP * sizeof(scene_seg_t), MALLOC_CAP_SPIRAM);
    if (!s_depth || !s_stamp || !s_tris || !s_lines) {
        ESP_LOGE(TAG, "scene buffer allocation failed (depth=%p stamp=%p tris=%p lines=%p)",
                 s_depth, s_stamp, s_tris, s_lines);
        return;
    }
    // One-time stamp clear so no garbage cell matches the first
    // live frame tag (1). The depth buffer needs no init — a cell is
    // only ever read after its stamp says it was written this frame.
    memset(s_stamp, 0, SCENE_PIXELS * sizeof(uint8_t));
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

    uint8_t const frame = s_frame;
    int const idx = direct_565_logical_index(lx, y_top);
    uint16_t* fp  = s_fb    + idx;
    uint16_t* zp  = s_depth + idx;
    uint8_t*  sp  = s_stamp + idx;
    float     d   = As * (float)lx + Bs * (float)y_top + Cs;
    int       cnt = y_bot - y_top + 1;
    while (cnt-- > 0) {
        int di = (int)d;
        if (di < 0) di = 0;                      // sub-pixel edge overshoot guard
        uint16_t const stored = (*sp == frame) ? *zp : 0;
        if ((uint16_t)di > stored) {
            *zp = (uint16_t)di;
            *sp = frame;
            *fp = packed;
        }
        fp--; zp--; sp--;
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

    uint8_t const frame  = s_frame;
    int     const ptr_dx = (sx > 0) ? DISPLAY_RAW_STRIDE : -DISPLAY_RAW_STRIDE;
    int     const ptr_dy = (sy > 0) ? -1 : 1;

    int const idx = direct_565_logical_index(x0, y0);
    uint16_t* fp  = s_fb    + idx;
    uint16_t* zp  = s_depth + idx;
    uint8_t*  sp  = s_stamp + idx;
    int       lx  = x0;
    int       ly  = y0;

    while (1) {
        if (lx >= 0 && lx < DISPLAY_LOG_W && ly >= 0 && ly < DISPLAY_LOG_H) {
            int di = (int)d;
            if (di < 0) di = 0;
            uint16_t const stored = (*sp == frame) ? *zp : 0;
            if ((uint16_t)di >= stored) *fp = packed;
        }
        if (lx == x1 && ly == y1) break;
        int const e2 = 2 * err;
        if (e2 > -dy) { err -= dy; lx += sx; fp += ptr_dx; zp += ptr_dx; sp += ptr_dx; }
        if (e2 <  dx) { err += dx; ly += sy; fp += ptr_dy; zp += ptr_dy; sp += ptr_dy; }
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

void scene_render(se_render_mode_t mode) {
    (void)mode;   // only SE_RENDER_ZBUFFER ships today

    scene_cull_pass();    // frustum cull (opt-in; no-op when disabled)
    scene_order_pass();   // front-to-back order (opt-in; no-op when disabled)

    // Triangles first (per-pixel z-test makes their order irrelevant), in
    // submission order -- identical to the old immediate path.
    for (int i = 0; i < s_tri_n; i++) {
        scene_raster_tri(s_tris[i].v[0], s_tris[i].v[1], s_tris[i].v[2], s_tris[i].packed);
    }
    // Then the wireframe edges, z-tested against the depth the tris wrote.
    for (int i = 0; i < s_line_n; i++) {
        scene_raster_line(s_lines[i].v[0], s_lines[i].v[1], s_lines[i].packed);
    }
    s_tri_n  = 0;
    s_line_n = 0;
}

void scene_flush(void) {
    scene_render(SE_RENDER_ZBUFFER);
}
