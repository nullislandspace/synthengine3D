// =====================================================================
//  SynthEngine3D  --  scene lighting (se_light.h)
// ---------------------------------------------------------------------
//  One positional light, applied per triangle at submit time. The
//  public contract -- what brightness means, why there are no shadows
//  and no falloff, what two_sided does -- is documented in
//  include/se_light.h; this file is the arithmetic.
// =====================================================================

#include <math.h>

#include "se_light.h"
#include "se_light_internal.h"

bool se_light_is_on = false;

static se_light_t s_light;

// Pre-computed from s_light.brightness on each set, so the per-triangle
// path is one multiply-add rather than a subtraction as well:
//   shade = s_floor + s_range * d
static float s_floor = 1.0f;   // 1 - brightness: the global-illumination term
static float s_range = 0.0f;   // brightness: the directional term's span

void se_light_set(se_light_t const* light) {
    if (light == NULL) {
        se_light_is_on = false;
        return;
    }
    s_light = *light;
    // Clamp rather than reject: a brightness slightly out of range is a
    // caller's rounding, not a reason to drop the light and leave the
    // scene silently flat.
    if (s_light.brightness < 0.0f) s_light.brightness = 0.0f;
    if (s_light.brightness > 1.0f) s_light.brightness = 1.0f;
    s_range = s_light.brightness;
    s_floor = 1.0f - s_light.brightness;
    se_light_is_on = true;
}

bool se_light_get(se_light_t* out) {
    if (!se_light_is_on) return false;
    if (out != NULL) *out = s_light;
    return true;
}

uint32_t se_light_shade_tri(uint32_t argb,
                            float x0, float y0, float z0,
                            float x1, float y1, float z1,
                            float x2, float y2, float z2,
                            float camx, float camy, float camz) {
    // Geometric normal: the cross product of two edges. Its length is
    // twice the triangle's area, which is irrelevant here -- only the
    // direction matters, and the normalisation below divides it out.
    float const ux = x1 - x0, uy = y1 - y0, uz = z1 - z0;
    float const vx = x2 - x0, vy = y2 - y0, vz = z2 - z0;
    float const nx = uy * vz - uz * vy;
    float const ny = uz * vx - ux * vz;
    float const nz = ux * vy - uy * vx;

    // Face centroid -> the light. A position, not a direction, so this
    // is recomputed per face and two faces of the same object can meet
    // the light at different angles.
    float const fcx = (x0 + x1 + x2) * (1.0f / 3.0f);
    float const fcy = (y0 + y1 + y2) * (1.0f / 3.0f);
    float const fcz = (z0 + z1 + z2) * (1.0f / 3.0f);
    float const lx = s_light.x - fcx;
    float const ly = s_light.y - fcy;
    float const lz = s_light.z - fcz;

    float const n2 = nx * nx + ny * ny + nz * nz;
    float const l2 = lx * lx + ly * ly + lz * lz;
    // A degenerate triangle has no normal, and a light sitting exactly
    // on a face has no direction. Either way there is nothing to shade
    // by, so hand back the colour unchanged instead of dividing by ~0.
    float const denom2 = n2 * l2;
    if (denom2 < 1e-24f) {
        return argb;
    }

    // cos(angle between the face and the direction to the light). One
    // sqrtf for both normalisations: |n||l| == sqrt(n2 * l2).
    float ndotl = nx * lx + ny * ly + nz * lz;
    if (s_light.two_sided) {
        // Orient the normal towards the eye, so whichever side the
        // camera sees is the side that gets lit and the result does not
        // depend on the game's winding. Flipping the normal negates the
        // dot product, so this needs no second normalisation.
        float const ex = camx - fcx, ey = camy - fcy, ez = camz - fcz;
        if (nx * ex + ny * ey + nz * ez < 0.0f) {
            ndotl = -ndotl;
        }
    }
    float d = ndotl / sqrtf(denom2);
    if (d < 0.0f) d = 0.0f;   // turned away: the global term alone

    float const shade = s_floor + s_range * d;

    // Scale RGB, keep alpha. shade is in [0, 1], so no channel can
    // exceed its input and the result needs no saturation.
    uint32_t const a = (argb >> 24) & 0xFF;
    uint32_t const r = (uint32_t)((float)((argb >> 16) & 0xFF) * shade);
    uint32_t const g = (uint32_t)((float)((argb >> 8) & 0xFF) * shade);
    uint32_t const b = (uint32_t)((float)(argb & 0xFF) * shade);
    return (a << 24) | (r << 16) | (g << 8) | b;
}
