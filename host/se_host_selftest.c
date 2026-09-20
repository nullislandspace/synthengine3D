// =====================================================================
//  SynthEngine3D  --  host harness self-test (make -C host check)
// ---------------------------------------------------------------------
//  The smallest possible user of the harness, and its regression test:
//  implements the five hooks, drives the camera, submits one of every
//  primitive and checks what arrived. It doubles as the example a game's
//  own checker starts from -- see docs/testing.md.
// =====================================================================

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "se_host.h"
#include "synthengine3d.h"

static int tris, ttris, lines, points;

void se_host_tri(float const xyz[9], bool textured, uint32_t argb, uint32_t flags) {
    (void)xyz; (void)argb; (void)flags;
    if (textured) ttris++; else tris++;
}
void se_host_line(float const a[3], float const b[3], uint32_t argb) { (void)a; (void)b; (void)argb; lines++; }
void se_host_point(float const p[3], uint32_t argb) { (void)p; (void)argb; points++; }

int main(void) {
    render_set_camera_6dof(0, 0, 0, 0, 0, 0);
    // Identity basis: a point 10 ahead projects to the screen centre.
    float const w[3] = {0, 0, 10}; float c[3]; float sx, sy;
    se_host_to_camera(w, c);
    assert(fabsf(c[0]) < 1e-6f && fabsf(c[1]) < 1e-6f && fabsf(c[2] - 10.0f) < 1e-6f);
    se_host_project(c, &sx, &sy);
    assert(fabsf(sx - RENDER_HALF_W) < 1e-3f && fabsf(sy - RENDER_HORIZON_Y) < 1e-3f);

    // A yaw of 90 degrees puts +x in front of the camera.
    render_set_camera_6dof(0, 0, 0, (float)M_PI / 2.0f, 0, 0);
    float const wx[3] = {10, 0, 0};
    se_host_to_camera(wx, c);
    assert(fabsf(c[2] - 10.0f) < 1e-4f);

    scene_tri(0,0,1, 1,0,1, 0,1,1, 0xFFFFFFFFu, 0);
    scene_line(0,0,1, 1,1,1, 0xFF00FF00u);
    scene_point(0,0,1, 0xFFFF0000u);
    se_texture_t* t = se_texture_load("whatever.png", 0);
    assert(t != NULL && t->w == 64);
    se_tex_vertex_t v[3] = {{0,0,1,0,0},{1,0,1,1,0},{0,1,1,0,1}};
    scene_textured_tri(v, t, 0);
    se_texture_unload(t);

    se_light_set(&(se_light_t){.x=1,.y=2,.z=3,.brightness=0.5f,.two_sided=false});
    se_light_t got; assert(se_light_get(&got) && got.brightness == 0.5f);

    assert(tris == 1 && ttris == 1 && lines == 1 && points == 1);
    printf("host harness self-test OK (tris %d ttris %d lines %d points %d)\n", tris, ttris, lines, points);
    return 0;
}
