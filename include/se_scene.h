#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  3D scene pipeline
// ---------------------------------------------------------------------
//  The software 3D renderer: a per-pixel z-buffered rasterizer plus the
//  pinhole camera + projection it draws through. Games submit world-space
//  triangles / wireframe edges; the engine projects, depth-tests and
//  rasterizes them. The engine knows nothing about game objects — the
//  game iterates its own world and calls scene_tri / scene_line. Part of
//  the semver'd public surface (see se_version.h); projection constants
//  are overridable defaults in se_config.h.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "pax_gfx.h"
#include "se_config.h"   // RENDER_* projection params (overridable)

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

// Which rasterization algorithm scene_render() uses. Only the per-pixel
// z-buffer ships today; the enum exists so a game can select a different
// pipeline later (painter's, tiled, ...) with no change to its submit
// calls. SE_RENDER_DEFAULT is the engine's recommended choice.
typedef enum {
    SE_RENDER_ZBUFFER = 0,           // per-pixel reciprocal-z depth test
    SE_RENDER_DEFAULT = SE_RENDER_ZBUFFER,
} se_render_mode_t;

// Allocate the depth buffer, frame-stamp plane and the deferred triangle
// + edge buffers (all PSRAM). Call once at boot, before the first frame.
void scene_init(void);

// Begin a frame's 3D pass: bind the framebuffer, advance the frame
// stamp (logically emptying the depth buffer), reset the deferred
// triangle + edge lists. Call once per frame before any scene_tri /
// scene_line.
void scene_begin(pax_buf_t* fb);

// Submit a world-space triangle. Projected with the current camera and
// accumulated; rasterized at scene_render(). `argb` is ARGB8888.
void scene_tri(float x0, float y0, float z0,
               float x1, float y1, float z1,
               float x2, float y2, float z2, uint32_t argb);

// Submit a world-space wireframe edge. Projected + accumulated; drawn at
// scene_render() after every triangle.
void scene_line(float x0, float y0, float z0,
                float x1, float y1, float z1, uint32_t argb);

// Rasterize the whole accumulated frame (triangles then edges) with the
// chosen algorithm, then empty the lists. Call once after all geometry
// for the frame has been submitted. The central cull + order passes run
// here (opt-in via scene_set_options; off by default). SE_RENDER_ZBUFFER
// reproduces the legacy per-pixel-depth output exactly.
void scene_render(se_render_mode_t mode);

// Back-compat alias for scene_render(SE_RENDER_ZBUFFER).
void scene_flush(void);

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
