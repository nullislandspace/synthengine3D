#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  scene lighting
// ---------------------------------------------------------------------
//  One optional positional light for the scene pipeline. When a light is
//  set, scene_tri() shades every triangle it accepts: it derives the
//  face's geometric normal from the world-space vertices it was handed,
//  works out how squarely that face meets the light, and scales the
//  colour the game passed before storing it. When no light is set the
//  engine stores that colour untouched -- exactly as it did before this
//  facility existed -- so lighting costs nothing until a game asks for
//  it. Part of the semver'd public surface (see se_version.h).
//
//  ONCE PER FRAME, AT SUBMIT TIME. The shade is computed per triangle in
//  scene_tri, not per pixel in the rasterizer: a triangle is one flat
//  colour on screen, so per-pixel work would buy nothing. That also
//  keeps the result available as a per-face quantity for later passes
//  (texturing) to modulate, rather than something buried in the inner
//  rasterizer loop.
//
//  WHAT THIS IS NOT: there are no shadows. The light reaches every face
//  that points at it, whether or not another object stands in between.
//  There is also no distance falloff -- only the angle between the face
//  and the direction to the light matters, so moving the light further
//  away changes which faces are lit, never how brightly. One light, no
//  specular term, no per-vertex/smooth shading (a triangle is flat by
//  construction, see se_scene.h).
//
//  Wireframe edges (scene_line) are never lit: a line has no surface and
//  therefore no normal. An outline keeps the colour the game gave it.
// =====================================================================

#include <stdbool.h>

// A single positional light.
//
// `brightness` is the DIRECTIONAL SHARE of the scene's total
// illumination, as a fraction in 0..1 (0.7 = 70%). The rest is global
// illumination -- a flat fill that reaches every surface from every
// direction. So, with d = how squarely the face meets the light
// (1 = square on, 0 = edge-on or turned away):
//
//     shade = (1 - brightness)  +  brightness * d
//
// A face square-on to the light gets shade 1.0: the game's colour, at
// full strength. A face turned away gets (1 - brightness): the global
// term alone. Hence:
//
//     brightness = 0.0   no directional light at all; every face at
//                        full colour, i.e. the unlit look.
//     brightness = 0.4   faces turned away sit at 60% of their colour;
//                        a gentle, readable amount of modelling.
//     brightness = 1.0   pure directional light, no fill whatsoever;
//                        faces turned away go black.
//
// Out-of-range values are clamped by se_light_set().
//
// `position` is in world units, the same space scene_tri's vertices are
// in. It is a point in the world, not a direction: the engine takes the
// direction from each face's centroid to this point, so the lit side of
// an object changes as either the light or the object moves.
//
// `two_sided` decides how the face's normal is oriented. The engine gets
// its normal from the cross product of the triangle's edges, whose sign
// depends on the winding the game used -- and the engine deliberately
// does not mandate a winding (see se_scene.h: back-face culling belongs
// to the game, which is free to wind either way).
//
//     true   Flip the normal towards the camera when it points away, so
//            the face the viewer can see is the face that gets lit. Any
//            winding works, and a game that submits both sides of an
//            open surface (a sail, a flag) sees both sides lit. This is
//            the option to pick if you are unsure.
//     false  Use the cross-product normal as-is. Correct only for
//            CCW-outward winding, and a shade below the global term is
//            impossible either way -- a back face turned away from the
//            light simply sits at the floor. Marginally cheaper (it
//            skips one dot product per triangle).
typedef struct {
    float x, y, z;     // light position, world units
    float brightness;  // directional share of total illumination, 0..1
    bool  two_sided;   // orient each normal towards the camera
} se_light_t;

// Set the scene light, or pass NULL to switch lighting off. The struct
// is copied; nothing retains the pointer. Takes effect from the next
// scene_tri() -- so call it before submitting a frame's geometry, not in
// the middle, or the frame will be lit two different ways.
//
// A light with brightness 0 is not the same as no light: it is a light
// whose directional share happens to be zero. Both leave every face at
// full colour, but the former still pays the per-triangle normal. Pass
// NULL when a game has no lighting at all.
void se_light_set(se_light_t const* light);

// Read the current light into *out. Returns false and leaves *out alone
// when lighting is off. `out` may be NULL to test only whether a light
// is set.
bool se_light_get(se_light_t* out);
