#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  3D scene pipeline
// ---------------------------------------------------------------------
//  The software 3D renderer: a per-pixel z-buffered rasterizer plus the
//  pinhole camera + projection it draws through. Games submit world-space
//  triangles / wireframe edges; the engine projects, depth-tests and
//  rasterizes them. The engine knows nothing about game objects — the
//  game iterates its own world and calls scene_tri / scene_line. Part of
//  the versioned public surface (see se_version.h); projection constants
//  are overridable defaults in se_config.h.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "pax_gfx.h"
#include "se_config.h"   // RENDER_* projection params (overridable)
#include "se_texture.h"  // se_texture_t (scene_textured_tri)

// Depth-buffered 3D scene pipeline.
//
// Replaces the old per-object painter's algorithm in render.c. Every
// 3D object (obstacles, the ship) submits its geometry as world-space
// triangles and wireframe edges; the scene module projects it, depth-
// tests it per pixel against a z-buffer, and rasterizes it. Draw order
// no longer matters — the depth test resolves visibility, so objects
// can be submitted in any order and the result is per-pixel correct
// (stacked, straddling and interpenetrating geometry all just work).
//
// Depth is stored as a scaled reciprocal-z: larger value = nearer.
// The depth buffer is never bulk-cleared: a parallel per-pixel frame
// stamp marks which depths belong to the current frame, so a stale
// pixel reads as infinitely far and scene_begin costs one counter
// increment instead of a full-screen memset.
//
// Deferred submission (ER): scene_tri / scene_line do NOT draw on call.
// They project the world-space geometry with the current camera and
// accumulate it into per-frame triangle / edge lists. scene_render()
// then rasterizes the whole frame at once: triangles first (per-pixel
// z-tested, so their order is irrelevant), then wireframe edges (z-tested
// with a small bias so an edge wins against the coplanar face it outlines
// but still loses to genuinely nearer geometry). Holding the whole frame
// lets the engine own the algorithm (z-buffer today; painter's / sorted /
// tiled later) and cull + order the geometry centrally against the FOV /
// resolution — without any game call site changing. Those cull + order
// passes (se_scene_options_t / scene_set_options) are opt-in and default
// OFF, so the renderer is byte-identical to the old hybrid-immediate
// pipeline until a game enables them.

// Which algorithm scene_render() uses to resolve the frame. Selected per
// call, so a game can switch renderers live (between frames) or pick a
// different one per screen. Both built-ins consume the SAME submitted
// geometry and produce the SAME image -- they differ only in loop order
// and therefore in what they cost:
//
//   SE_RENDER_ZBUFFER  primitive-driven. Walks triangles, and for each
//                      one walks the pixels it covers, depth-testing per
//                      pixel. Cost scales with the summed triangle AREA,
//                      i.e. covered pixels x overdraw: a pixel under N
//                      overlapping triangles is visited N times.
//
//   SE_RENDER_RAYCAST  pixel-driven. Bins triangles into screen tiles,
//                      then for each pixel of a non-empty tile casts one
//                      primary ray, keeps the nearest hit, and writes the
//                      pixel ONCE. Cost is independent of overdraw, but it
//                      pays edge tests on every pixel of a non-empty tile,
//                      including ones the ray misses.
//
// Neither is universally faster -- the crossover depends on the scene's
// overdraw and how tightly its geometry packs into tiles, so a game (or a
// different game reusing the engine) should measure both with
// scene_raster_stats(). SE_RENDER_DEFAULT is the safe general choice.
//
// Why the two agree pixel-for-pixel: for PRIMARY rays through a pinhole
// camera, "the ray through pixel p hits triangle T first" is exactly "p is
// inside T's projection and T holds the largest 1/z there" -- which is what
// the depth test computes. So the raycaster is a reordering of the same
// visible-surface problem, not a different one, and it needs no world-space
// geometry: it casts against the projected (sx, sy, w) triangles. A ray that
// hits nothing writes no pixel, so whatever the game already painted (a
// backdrop, a PPA composite) shows through untouched -- identical to the
// rasterizer, which only touches covered pixels.
//
// Values above SE_RENDER_BUILTIN_COUNT are handles returned by
// se_renderer_register() (see below).
typedef enum {
    SE_RENDER_ZBUFFER = 0,           // per-pixel reciprocal-z depth test
    SE_RENDER_RAYCAST = 1,           // tiled primary-ray cast, zero overdraw
    SE_RENDER_DEFAULT = SE_RENDER_ZBUFFER,
    SE_RENDER_BUILTIN_COUNT = 2,     // first handle se_renderer_register() hands out
    SE_RENDER_MAX = 8,               // renderer table size (built-ins + custom)
} se_render_mode_t;

// Allocate the depth buffer, frame-stamp plane and the deferred triangle
// + edge buffers (all PSRAM). Call once at boot, before the first frame.
void scene_init(void);

// Begin a frame's 3D pass: bind the framebuffer, advance the frame
// stamp (logically emptying the depth buffer), reset the deferred
// triangle + edge lists. Call once per frame before any scene_tri /
// scene_line.
void scene_begin(pax_buf_t* fb);

// --- Quarter-resolution rendering ------------------------------------
//
// scene_set_render_scale(2) renders the next frames at half the width
// and half the height -- a quarter of the pixels, so about a quarter of
// the rasterizing, which is what a fill-bound scene pays for. Nothing
// else changes: the camera, the RENDER_* projection, the viewport (still
// given in full-screen pixels), culling, clipping and lighting all work
// in full-screen coordinates as before. Only the projected positions
// are halved, so the target's pixel (i, j) is exactly what full
// resolution would draw at (2i, 2j) -- every other pixel of every other
// line, packed without the gaps.
//
// The fb given to scene_begin() must then be a buffer of
// DISPLAY_LOG_W/2 x DISPLAY_LOG_H/2 logical pixels in the display's
// format and orientation (e.g. an se_ppa_layer_t allocated that size),
// which the game scales up onto the screen afterwards
// (se_ppa_blit_scaled). Lines and points stay one target pixel wide, so
// two screen pixels. The raycast renderer falls back to the z-buffer at
// quarter resolution. Switch freely between frames: the scale is
// latched by scene_begin(). 1 (the default) is full resolution; any
// other value than 2 means 1.
void scene_set_render_scale(int div);
int  scene_render_scale(void);  // the scale requested for the next frame

// --- Triangle flags ---------------------------------------------------
//
// A bit mask passed with every triangle, flat or textured. 0 is the
// plain triangle: depth-tested, and lit if a light is set. Bits not
// defined here are reserved -- pass them as 0, so that a later engine
// can give them a meaning without changing what existing calls do.
//
//   SE_TRI_EMISSIVE  Never lit: the triangle keeps the exact colour (or
//                    texels) it was given, at full strength, whatever
//                    se_light_set() says. For things that give off light
//                    rather than reflect it -- engine flames, lamps,
//                    screens -- and for geometry the game has already
//                    shaded itself. It costs less than a lit triangle,
//                    since the per-face normal is skipped too.
#define SE_TRI_EMISSIVE  (1u << 0)

// Submit a world-space triangle. Projected with the current camera and
// accumulated; rasterized at scene_render(). `argb` is ARGB8888;
// `flags` is a mask of SE_TRI_* bits (0 for a plain triangle).
void scene_tri(float x0, float y0, float z0,
               float x1, float y1, float z1,
               float x2, float y2, float z2, uint32_t argb, uint32_t flags);

// Submit a world-space wireframe edge. Projected + accumulated; drawn at
// scene_render() after every triangle.
void scene_line(float x0, float y0, float z0,
                float x1, float y1, float z1, uint32_t argb);

// Submit a world-space point: one pixel of `argb`, for starfields,
// sparks and the like. Projected like any vertex, so it moves correctly
// with the camera; unlit, like an edge. Depth-tested against the
// triangles (hidden behind them) but never written to the depth buffer.
// Drawn after the edges. Points behind the near plane or outside the
// viewport are dropped at submit time. A star "at infinity" is a point
// at the camera position plus a fixed direction times a large distance
// (anything up to ~30000 units still depth-tests correctly).
void scene_point(float x, float y, float z, uint32_t argb);

// --- Textured triangles -------------------------------------------------
//
// A third primitive, alongside scene_tri and scene_line (both unchanged):
// a triangle that samples a texture (se_texture.h) instead of carrying
// one flat colour. Submitted, projected and deferred the same way, into
// a list of its own, and rasterized after the flat triangles and before
// the edges, depth-tested per pixel against both. So textured and flat
// geometry occlude each other correctly in either order of submission,
// and outlines still draw over textured faces.
//
// Each vertex carries a texture coordinate (u, v) as well as its world
// position. (0, 0) is the texture's top-left corner and (1, 1) its
// bottom-right. Values outside 0..1 repeat the texture, so a plate can
// tile across a large face. Mapping is PERSPECTIVE-CORRECT: u, v are
// interpolated as u/z and v/z and divided per pixel, so a texture on a
// face seen at an angle does not swim as the face turns. Sampling is
// nearest-texel, with no filtering and no mipmaps.
//
// Lighting (se_light.h) applies exactly as it does to scene_tri: the
// same per-face shade, computed once here at submit time from the same
// world-space normal, modulates every texel of the face. Unlike a flat
// triangle, where the shade is folded into the one colour, a textured
// face keeps it as a separate factor quantised to 1/32 steps. That is
// finer than an RGB565 channel can show at full brightness anyway.
// `flags` takes the same SE_TRI_* bits as scene_tri; SE_TRI_EMISSIVE
// draws the texels unshaded.
//
// `tex` NULL drops the triangle. The texture must stay loaded until the
// frame that used it has been rasterized (see se_texture_unload).
typedef struct {
    float x, y, z;   // world position
    float u, v;      // texture coordinate; 0..1 spans the texture once
} se_tex_vertex_t;

void scene_textured_tri(se_tex_vertex_t const v[3], se_texture_t const* tex, uint32_t flags);

// Rasterize the whole accumulated frame (triangles then edges) with the
// chosen algorithm, then empty the lists. Call once after all geometry
// for the frame has been submitted. The central cull + order passes run
// here (opt-in via scene_set_options; off by default). SE_RENDER_ZBUFFER
// reproduces the legacy per-pixel-depth output exactly.
void scene_render(se_render_mode_t mode);

// Back-compat alias for scene_render(SE_RENDER_ZBUFFER).
void scene_flush(void);

// Two-phase form of scene_render(), for overlapping the geometry-only work
// with other engine activity. scene_prepare() runs the cull + order passes
// and touches NO framebuffer pixels — only the deferred geometry lists — so
// it is safe to run concurrently with a hardware blit that writes the
// framebuffer (e.g. a PPA backdrop composite). scene_rasterize() then paints
// the prepared geometry and must run after that blit has completed. Submit
// all geometry, call scene_prepare(), then scene_rasterize() once the
// framebuffer is ready. scene_render() is exactly these two back-to-back and
// stays the simple choice when there's nothing to overlap; the output is
// identical either way.
void scene_prepare(se_render_mode_t mode);
void scene_rasterize(se_render_mode_t mode);

// Diagnostics for the most recent scene_rasterize(): the geometry counts
// actually rasterized (post-cull) and the per-phase wallclock split between
// filled triangles and wireframe edges, in microseconds. For profiling how
// scene render time divides; any pointer may be NULL.
void scene_raster_stats(int* tri_n, int* line_n, int64_t* tri_us, int64_t* line_us);

// The same diagnostic for the textured-triangle pass: how many were
// rasterized in the most recent scene_rasterize() (post-cull), and how
// long that pass took. Kept apart from scene_raster_stats(), whose
// tri_n / tri_us go on meaning flat triangles only. Either pointer may
// be NULL.
void scene_textured_stats(int* ttri_n, int64_t* ttri_us);

// The same for points: how many were drawn in the most recent
// scene_rasterize() and how long the point pass took. Either pointer may
// be NULL.
void scene_point_stats(int* pt_n, int64_t* pt_us);

// --- Camera & projection ---------------------------------------------
//
// The scene projects through a single module-global six-degree-of-freedom
// pinhole camera, set once per frame before submitting geometry. Position
// is the eye in world units; orientation is yaw / pitch / roll in radians,
// applied in that order: yaw about world-up (+y), then pitch about the
// camera's right (+x), then roll about forward (+z). At zero orientation
// the camera looks straight down +z with +y up and +x right — identical
// to the legacy fixed camera. The projection uses the RENDER_* constants
// from se_config.h (focal length / principal point — i.e. the FOV;
// overridable per game). The engine caches the rotation basis on each
// set, so the trig runs once per frame, not once per vertex.
typedef struct {
    float x, y, z;            // eye position (world units)
    float yaw, pitch, roll;   // orientation (radians)
} render_camera_t;

// Set / read the scene camera. Call once per frame before the first
// scene_tri / scene_line. render_set_camera_6dof() sets the full pose;
// render_set_camera(x, y) is the legacy shorthand for an eye on the
// z = 0 plane looking straight down +z (zero orientation), which projects
// byte-for-byte like the pre-6DOF engine.
void            render_set_camera(float x, float y);
void            render_set_camera_6dof(float x, float y, float z,
                                       float yaw, float pitch, float roll);
render_camera_t render_camera(void);

// Project a world point (x_w, y_w, z_w) to screen pixels through the
// current camera pose. At zero orientation: y = 0 is the ground plane,
// +y up, +z forward (away from the camera). Out values are in pax logical
// pixels. (scene_tri / scene_line project internally; this is for game
// code that needs to project a point itself — e.g. drawing a floor
// shadow.)
void render_project(float x_w, float y_w, float z_w, float* out_sx, float* out_sy);

// --- Viewport ---------------------------------------------------------
//
// Restrict every pixel the scene writes to a rectangle. Triangles, edges
// and the depth plane are all clipped to it, and the frustum-cull pass
// (scene_set_options) culls against it rather than against the whole
// screen, so a smaller viewport tightens culling for free.
//
// This is for a game that frames its 3D view inside a fixed border -- a
// cockpit surround, a letterbox, a split screen, a dashboard along the
// bottom. Without it, drawing the whole screen and then painting the
// border over the top pays the fill twice: once to rasterize pixels
// nobody will ever see, and again to cover them. Those pixels are
// usually the expensive ones, too -- a dashboard sits over the nearest,
// most overdrawn band of a ground-plane scene.
//
// Coordinates are pax LOGICAL pixels, the same space scene_tri projects
// into, and the rect is clamped to the framebuffer, so an oversized or
// partly-negative one is safe. An empty or inverted rect collapses to a
// single pixel: the frame goes blank, which is diagnosable, rather than
// silently drawing nothing or everything.
//
// Defaults to the whole framebuffer; passing NULL restores that. It is a
// persistent setting, NOT reset by scene_begin() -- how a game frames its
// view is a property of the game, not of the frame.
//
// The viewport clips; it does not scale or re-centre. The projection is
// still the RENDER_* pinhole about RENDER_HALF_W / RENDER_HORIZON_Y, so
// a viewport that is not centred on those shows an off-centre crop of
// the same image rather than a re-framed one. A game that wants the
// vanishing point inside its window moves it with the RENDER_* overrides
// in se_config.h.
typedef struct {
    int x, y, w, h;   // logical pixels; w/h are sizes, not far edges
} se_viewport_t;

void          scene_set_viewport(se_viewport_t const* vp);
se_viewport_t scene_viewport(void);

// --- Optional render passes ------------------------------------------
//
// Two opt-in optimizations scene_render() can run before rasterizing.
// Both are OUTPUT-NEUTRAL: they change only how fast the frame is drawn,
// never the pixels, so they are always safe to toggle live (between
// frames or mid-run). Both default OFF — the engine reproduces the
// legacy pipeline exactly until a game opts in. They are independent
// features with opposite cost profiles, so measure each on its own.
//
// (Back-face culling is intentionally absent: the engine sees only
// anonymous projected triangles, whereas a game's objects know their
// face normals and can cull back faces at emit time — cheaper, and safe
// for any winding. Keep back-face culling in the game, not here.)
typedef struct {
    // Drop triangles / edges that project entirely off-screen before
    // rasterizing. Cheap O(n) screen-space test; a near-pure win when the
    // world submits geometry outside the FOV (far objects before they
    // swing into view). Runs after projection, so it respects the camera
    // pose + FOV automatically (the screen rect IS the projected frustum).
    bool frustum_cull;
    // Sort triangles front-to-back so occluded fragments fail the depth
    // test with no framebuffer write (early-z). Costs an O(n log n) sort
    // per frame: wins under heavy overdraw (dense scenes), can lose under
    // light overdraw. Edges are unaffected (they never write depth).
    bool depth_order;
} se_scene_options_t;

// Set / read the optional-pass configuration. Takes effect from the next
// scene_render(). Passing NULL resets to defaults (both OFF). Toggling
// one pass independently is a get-modify-set on the returned struct.
void               scene_set_options(se_scene_options_t const* opts);
se_scene_options_t scene_get_options(void);

// --- Pluggable renderers ---------------------------------------------
//
// The two built-ins are just the two renderers the engine ships with;
// the pipeline itself is a seam. A game can register its own renderer
// and select it exactly like a built-in, which is the point of the
// engine being reusable: a different game with different geometry (or a
// different visual style -- cel shading, dithering, a depth-cued fog
// pass) can replace the resolve step without touching a single
// scene_tri / scene_line call site.
//
// A renderer is two callbacks mirroring scene_prepare / scene_rasterize:
//
//   prepare()    Geometry-only. May reorder, compact or index the
//                deferred lists, and build whatever acceleration
//                structure it needs. MUST NOT touch framebuffer pixels
//                -- the game is allowed to run this concurrently with a
//                hardware blit that is writing the framebuffer.
//   rasterize()  Paints. Runs after any such blit has completed.
//
// Both receive the `user` pointer given at registration. The engine's
// own cull / order passes (scene_set_options) run BEFORE prepare(), so a
// custom renderer inherits them for free and sees an already-culled list.
typedef struct {
    char const* name;                  // for logs / debug UI; not copied
    void      (*prepare)(void* user);  // geometry only, no pixels
    void      (*rasterize)(void* user);// paints
    void*       user;
} se_renderer_t;

// Register a renderer and get the handle to pass to scene_render() /
// scene_prepare() / scene_rasterize(). Returns SE_RENDER_DEFAULT if the
// table is full or `r` is malformed (both callbacks are required), so a
// caller that ignores the result still renders something sane. The
// se_renderer_t is copied; the name string and user pointer are not.
se_render_mode_t se_renderer_register(se_renderer_t const* r);

// Human-readable name of a renderer, for debug overlays and logs.
// Returns "?" for a handle that was never registered.
char const* se_renderer_name(se_render_mode_t mode);

// --- Geometry view (for custom renderers) ----------------------------
//
// What a renderer gets to work with. These are the projected, culled and
// (optionally) depth-ordered primitives for the current frame, plus the
// targets to resolve them into. Only meaningful inside a renderer's
// prepare() / rasterize() callback.
//
// Vertices are already in screen space: sx / sy in pax logical pixels and
// w = 1/z (LARGER IS NEARER), the quantity that interpolates linearly
// across a projected triangle. A renderer that wants encoded depth
// multiplies w by `depth_scale`.
//
// The depth plane is one uint32 per pixel, (frame_stamp << 16) | depth,
// indexed exactly like `fb` via direct_565_logical_index() from
// se_direct565.h. A cell counts only if its high half equals `frame`;
// anything else is a stale cell from an earlier frame and must read as
// infinitely far. This is what lets the engine skip clearing the depth
// buffer -- do not memset it, and do not assume it is zero.
typedef struct {
    float sx, sy, w;   // screen x/y (logical px) + 1/z depth
} se_vtx_t;

typedef struct {
    se_vtx_t v[3];
    uint16_t packed;   // RGB565, framebuffer-endian; write straight to fb
} se_tri_t;

typedef struct {
    se_vtx_t v[2];
    uint16_t packed;
} se_seg_t;

typedef struct {
    se_vtx_t v;
    uint16_t packed;
} se_pt_t;

// A textured triangle as the rasterizer sees it. Per vertex: screen
// position and w = 1/z as in se_vtx_t, plus the texture coordinate
// premultiplied by w and already scaled to texels (uw = u * tex->w * w).
// u*w and v*w are what interpolate linearly across the projected
// triangle; divide by the interpolated w to recover the texel.
typedef struct {
    float sx, sy, w;   // as se_vtx_t
    float uw, vw;      // texel u, v times w
} se_tvtx_t;

typedef struct {
    se_tvtx_t           v[3];
    se_texture_t const* tex;
    uint8_t             shade;  // light factor, 0..32 (32 = full colour)
} se_ttri_t;

typedef struct {
    se_tri_t const* tris;        // triangles for this frame (post-cull)
    int             tri_n;
    se_seg_t const* segs;        // wireframe edges (drawn after triangles)
    int             seg_n;
    uint16_t*       fb;          // RGB565 framebuffer
    uint32_t*       depth;       // (stamp << 16) | depth, indexed like fb
    uint16_t        frame;       // current frame stamp
    float           depth_scale; // multiply a vertex w by this to encode
    se_ttri_t const* ttris;      // textured triangles for this frame (post-cull)
    int             ttri_n;
    se_pt_t const*  pts;         // points for this frame (drawn after the edges)
    int             pt_n;
    int             scale;       // this frame's render scale: 2 = quarter resolution --
                                 // screen positions and fb are then half size (see above)
} se_geometry_t;

// Snapshot the current frame's geometry + targets. Call from inside a
// renderer callback; the pointers are owned by the engine and stay valid
// only for that call.
se_geometry_t se_scene_geometry(void);

// Draw this frame's textured triangles with the engine's own textured
// path, for a custom renderer that has no texturing of its own. Call it
// from the renderer's rasterize() AFTER the flat triangles have written
// their depth and BEFORE the edges, which is the order the built-in
// renderers use. It depth-tests against, and writes, the shared depth
// plane, so it composes with whatever the renderer drew.
void se_scene_raster_textured(void);

// Draw this frame's points with the engine's own point pass, for a custom
// renderer. Call it from rasterize() last, after the edges, as the
// built-in renderers do. It depth-tests against the shared depth plane
// but never writes it.
void se_scene_raster_points(void);
