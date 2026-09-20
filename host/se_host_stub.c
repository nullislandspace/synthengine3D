// =====================================================================
//  SynthEngine3D  --  host harness: the engine API, off the badge
// ---------------------------------------------------------------------
//  Implements the calls a game's scene and asset code makes, for a host
//  build (see se_host.h and docs/testing.md). Nothing is drawn: every
//  primitive goes to the game's checker through the se_host_* hooks.
//
//  The camera basis below is the same expansion as the engine's own
//  (se_scene.c, camera_basis): M = Ry(yaw) * Rx(pitch) * Rz(roll), with
//  the columns read out as right / up / forward. It is written out here
//  rather than shared because se_scene.c cannot compile on a host; both
//  live in this repo, so a change to one is visible against the other.
// =====================================================================

#include <math.h>
#include <stdlib.h>
#include "se_host.h"
#include "synthengine3d.h"

typedef struct {
    float right[3], up[3], fwd[3];
} basis_t;

static render_camera_t s_cam;
static basis_t         s_basis = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
static se_light_t      s_light;
static bool            s_light_on;

// --- Camera -----------------------------------------------------------

void render_set_camera_6dof(float x, float y, float z, float yaw, float pitch, float roll) {
    s_cam = (render_camera_t){x, y, z, yaw, pitch, roll};

    float const sy = sinf(yaw), cy = cosf(yaw);
    float const sp = sinf(pitch), cp = cosf(pitch);
    float const sr = sinf(roll), cr = cosf(roll);

    // Ry(yaw) * Rx(pitch) * Rz(roll), column by column.
    s_basis = (basis_t){
        .right = {cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr},
        .up    = {-cy * sr + sy * sp * cr, cp * cr, sy * sr + cy * sp * cr},
        .fwd   = {sy * cp, -sp, cy * cp},
    };
}

void render_set_camera(float x, float y) {
    render_set_camera_6dof(x, y, 0.0f, 0.0f, 0.0f, 0.0f);
}

render_camera_t render_camera(void) {
    return s_cam;
}

static float dot3(float const a[3], float const b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void se_host_to_camera(float const world[3], float out_cam[3]) {
    float const d[3] = {world[0] - s_cam.x, world[1] - s_cam.y, world[2] - s_cam.z};
    out_cam[0]       = dot3(s_basis.right, d);
    out_cam[1]       = dot3(s_basis.up, d);
    out_cam[2]       = dot3(s_basis.fwd, d);
}

void se_host_project(float const cam[3], float* out_sx, float* out_sy) {
    float const z = cam[2] < 0.01f ? 0.01f : cam[2];
    if (out_sx) *out_sx = RENDER_HALF_W + RENDER_FOCAL_LEN * cam[0] / z;
    if (out_sy) *out_sy = RENDER_HORIZON_Y - RENDER_FOCAL_LEN * cam[1] / z;
}

void render_project(float x_w, float y_w, float z_w, float* out_sx, float* out_sy) {
    float const world[3] = {x_w, y_w, z_w};
    float       cam[3];
    se_host_to_camera(world, cam);
    se_host_project(cam, out_sx, out_sy);
}

// --- Light -------------------------------------------------------------

void se_light_set(se_light_t const* light) {
    s_light_on = light != NULL;
    if (light) s_light = *light;
}

bool se_light_get(se_light_t* out) {
    if (s_light_on && out) *out = s_light;
    return s_light_on;
}

// --- Textures -----------------------------------------------------------
// Every load succeeds, with a blank 64x64 texture, so the game takes its
// textured paths exactly as it does on the badge (a failed load there is
// a missing file, which is the install's business, not a scene's).

se_texture_t* se_texture_load(char const* path, uint32_t flags) {
    (void)path;
    (void)flags;
    se_texture_t* t = calloc(1, sizeof(*t));
    if (t == NULL) return NULL;
    t->texels = calloc(64 * 64, sizeof(uint16_t));
    if (t->texels == NULL) {
        free(t);
        return NULL;
    }
    t->w = t->h  = 64;
    t->w_log2    = 6;
    t->mean_argb = 0xFF808080u;
    return t;
}

void se_texture_unload(se_texture_t* tex) {
    if (tex == NULL) return;
    free(tex->texels);
    free(tex);
}

// --- Primitives ----------------------------------------------------------

void scene_tri(float x0, float y0, float z0, float x1, float y1, float z1, float x2, float y2, float z2, uint32_t argb,
               uint32_t flags) {
    float const xyz[9] = {x0, y0, z0, x1, y1, z1, x2, y2, z2};
    se_host_tri(xyz, false, argb, flags);
}

void scene_textured_tri(se_tex_vertex_t const v[3], se_texture_t const* tex, uint32_t flags) {
    if (v == NULL || tex == NULL || tex->texels == NULL) return;  // as the engine
    float const xyz[9] = {v[0].x, v[0].y, v[0].z, v[1].x, v[1].y, v[1].z, v[2].x, v[2].y, v[2].z};
    se_host_tri(xyz, true, tex->mean_argb, flags);
}

void scene_line(float x0, float y0, float z0, float x1, float y1, float z1, uint32_t argb) {
    float const a[3] = {x0, y0, z0}, b[3] = {x1, y1, z1};
    se_host_line(a, b, argb);
}

void scene_point(float x, float y, float z, uint32_t argb) {
    float const p[3] = {x, y, z};
    se_host_point(p, argb);
}
