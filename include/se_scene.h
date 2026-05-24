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
// tiled later) and, in future, cull + order centrally against the FOV /
// resolution — without any game call site changing. Those cull/order
// passes ship as no-ops first (the renderer is otherwise behaviour-
// identical to the old hybrid-immediate pipeline).

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
// for the frame has been submitted. Central cull + order passes run here
// (no-ops in the current cut). SE_RENDER_ZBUFFER reproduces the legacy
// per-pixel-depth output exactly.
void scene_render(se_render_mode_t mode);

// Back-compat alias for scene_render(SE_RENDER_ZBUFFER).
void scene_flush(void);

// --- Camera & projection ---------------------------------------------
//
// The scene projects through a single module-global pinhole camera, set
// once per frame before submitting geometry. Kept as a struct so it can
// gain fields later (zoom, shake, look-ahead) without touching call
// sites. `x` is the lateral eye position, `y` the eye height; both are
// world units. The projection itself uses the RENDER_* constants from
// se_config.h (overridable per game).
typedef struct {
    float x;
    float y;
} render_camera_t;

// Set / read the scene camera. Call render_set_camera() once per frame
// before the first scene_tri / scene_line.
void            render_set_camera(float x, float y);
render_camera_t render_camera(void);

// Project a world point (x_w, y_w, z_w) to screen pixels using the
// current camera. y = 0 is the ground plane, +y up, +z forward (away
// from the camera). Out values are in pax logical pixels. (scene_tri /
// scene_line project internally; this is for game code that needs to
// project a point itself — e.g. drawing a floor shadow.)
void render_project(float x_w, float y_w, float z_w, float* out_sx, float* out_sy);
