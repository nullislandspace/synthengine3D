// =====================================================================
//  SynthEngine3D  --  minimal example app
// ---------------------------------------------------------------------
//  The smallest complete graceloader app on the engine: a spinning
//  triangle over a flat backdrop, with procedural music. It exercises
//  the run loop (se_run), the 3D renderer (se_scene) and the audio mixer
//  -- nothing else -- so you can see the shape of an app end to end.
//
//  Illustrative, not part of this game's build. To run it: make it your
//  app's single source, wire the engine in per docs/integration.md, and
//  build for the Tanmatsu.
// =====================================================================

#include <math.h>

#include "synthengine3d.h"   // the whole public API

static float s_angle = 0.0f;   // triangle spin, radians

// Once, after the engine has booted display + audio + scene. Start some
// music; the mixer takes ownership and frees it on shutdown.
static void on_init(void* user) {
    (void)user;
    // NULL config = the built-in synthwave preset; se_music_synthwave_preset()
    // returns it explicitly, and a game can pass its own se_music_config_t.
    audio_mixer_set_music(music_procedural_create(NULL, 0x5EED));

    // Optional, output-neutral scene passes (both default OFF). Frustum cull
    // is a near-pure win, so turn it on; depth order is an overdraw-dependent
    // trade-off, so leave it off until you've measured your scenes.
    scene_set_options(&(se_scene_options_t){
        .frustum_cull = true,
        .depth_order  = false,
    });
}

// Per frame: advance the spin. dt is seconds since the last frame.
static void on_update(float dt, void* user) {
    (void)user;
    s_angle += dt;
}

// Per frame, after the engine clears the backdrop: draw the 3D scene.
static void on_render(pax_buf_t* fb, void* user) {
    (void)user;
    render_set_camera(0.0f, 1.0f);          // eye at x=0, height 1, looking +z
    // Full pose if you need it (eye xyz + yaw/pitch/roll, radians):
    //   render_set_camera_6dof(0, 1, 0,  0, 0, 0);   // same as the line above
    scene_begin(fb);                         // start the frame's 3D pass

    // A triangle standing at z = 4, rotating about the vertical axis.
    float const c = cosf(s_angle);
    float const s = sinf(s_angle);
    scene_tri(-c, 0.0f, 4.0f - s,            // base-left
               c, 0.0f, 4.0f + s,            // base-right
               0.0f, 2.0f, 4.0f,             // apex
               0xFFFF31F1u);                 // magenta

    scene_render(SE_RENDER_ZBUFFER);         // rasterize the accumulated frame
}

// app_main hands the loop to the engine and never returns under
// graceloader (F1 reboots to the launcher).
void app_main(void) {
    static se_app_config_t const cfg = {
        .f1_exits      = true,               // engine handles F1 = exit
        .backdrop_argb = 0xFF101018u,        // dark blue-grey clear each frame
    };
    static se_app_callbacks_t const cb = {
        .on_init   = on_init,
        .on_update = on_update,              // the only required callback
        .on_render = on_render,
    };
    se_run(&cfg, &cb, NULL);                 // user context: none for this demo
}
