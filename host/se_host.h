#pragma once
// =====================================================================
//  SynthEngine3D  --  host harness: run a game's scene code on a PC
// ---------------------------------------------------------------------
//  A game's scene and asset code is ordinary C that calls scene_tri(),
//  scene_textured_tri(), scene_line(), scene_point(), the camera setters
//  and se_texture_load(). Compiled against this harness instead of the
//  engine, that same code runs on a desktop, in a plain host compiler,
//  with every primitive handed to the game's own checker rather than
//  drawn: so a test can look for geometry crossing the near plane, lists
//  that would overflow on the badge, objects intersecting each other, or
//  anything else that is the game's business -- in a second, with no
//  device attached and no display.
//
//  What the harness gives you:
//    * host/se_host_stub.c  -- the engine API, implemented for the host:
//      the same camera basis and projection as the badge (se_config.h),
//      an se_light that remembers what was set, textures that always
//      load (blank 64x64, so textured paths are taken), and primitives
//      forwarded to the hooks below;
//    * host/shims/          -- stand-ins for the ESP-IDF and PAX headers
//      the engine's public headers mention, so those headers compile on
//      a host: esp_log.h, esp_heap_caps.h, pax_gfx.h, and a host
//      synthengine3d.h umbrella that includes the REAL se_*.h headers,
//      so every type stays the engine's own.
//
//  What YOU supply: the five se_host_* hooks below, and a main() that
//  drives your scenes (set the camera, submit a frame, inspect what
//  arrived, repeat). See docs/testing.md for the compiler flags and a
//  worked example.
//
//  Not simulated: rasterizing, depth, PPA, audio, input, NVS, the run
//  loop. This is about what a frame CONTAINS, not what it looks like.
//  For what it looks like, render on the badge and hash the framebuffer.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- The hooks: every primitive, as submitted, before any clipping ----
//
// Coordinates are world space, in submission order, exactly as the game
// passed them. `xyz` holds three vertices (x, y, z, x, y, z, x, y, z).
// A textured triangle arrives with `textured` true and its texture
// coordinates dropped -- geometry is what a checker wants.
//
// All five are required: the harness calls them, your checker defines
// them, the linker insists.
void se_host_tri(float const xyz[9], bool textured, uint32_t argb, uint32_t flags);
void se_host_line(float const a[3], float const b[3], uint32_t argb);
void se_host_point(float const p[3], uint32_t argb);

// --- The camera, as the engine sees it --------------------------------
//
// The same maths the badge uses (basis M = Ry(yaw)·Rx(pitch)·Rz(roll),
// pinhole projection from RENDER_FOCAL_LEN / RENDER_HALF_W /
// RENDER_HORIZON_Y in se_config.h), so a checker can ask where a point
// lands on screen and how far in front of the camera it is.

// World point -> camera space (x right, y up, z forward/into the screen).
void se_host_to_camera(float const world[3], float out_cam[3]);

// Camera space -> screen pixels. z is clamped away from 0 exactly as the
// engine does, so a point behind the camera gives a defined, useless
// answer rather than an infinity: test cam[2] against RENDER_NEAR_CLIP_Z
// yourself if that matters.
void se_host_project(float const cam[3], float* out_sx, float* out_sy);

#ifdef __cplusplus
}
#endif
