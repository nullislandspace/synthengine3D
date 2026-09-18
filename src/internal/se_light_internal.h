#pragma once
// =====================================================================
//  SynthEngine3D  --  INTERNAL  --  lighting hook for the scene
// ---------------------------------------------------------------------
//  The seam between se_light.c (which owns the light) and se_scene.c
//  (which applies it in scene_tri). INTERNAL: not reachable from a
//  game's include path and not covered by the versioning contract.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

// True when a light is set. Read directly rather than through an
// accessor because scene_tri tests it once per triangle and lighting
// must cost nothing at all when it is off -- a load and a branch, no
// call. Written only by se_light_set().
extern bool se_light_is_on;

// The shade factor for one world-space triangle: (1 - brightness) +
// brightness * max(0, cos(angle to the light)), in [1 - brightness, 1].
// 1.0 for a degenerate triangle or a light sitting on the face. The
// textured path keeps this as a per-face factor; se_light_shade_tri
// folds it into a colour. Only call when se_light_is_on.
float se_light_face_shade(float x0, float y0, float z0,
                          float x1, float y1, float z1,
                          float x2, float y2, float z2,
                          float camx, float camy, float camz);

// Shade `argb` for one world-space triangle and return the result.
// Derives the face normal from the vertices, so the caller passes the
// same world-space coordinates it received. (camx, camy, camz) is the
// eye, needed only for the two_sided normal flip.
//
// Only call this when se_light_is_on. Alpha is preserved; the RGB
// channels are scaled by a factor in [1 - brightness, 1], so the result
// can never overflow the input.
uint32_t se_light_shade_tri(uint32_t argb,
                            float x0, float y0, float z0,
                            float x1, float y1, float z1,
                            float x2, float y2, float z2,
                            float camx, float camy, float camz);
