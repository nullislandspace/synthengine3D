// =====================================================================
//  SynthEngine3D  --  PPA backdrop example
// ---------------------------------------------------------------------
//  A hardware-composited backdrop using se_ppa: a sky FILL, a "sun" layer
//  BLIT that slides up and down, and a "hills" layer BLEND-keyed over the
//  top. It runs entirely in the on_backdrop hook -- the engine's per-frame
//  backdrop slot -- so the CPU is free for the 3D scene in on_render.
//
//  It shows the whole se_ppa shape: bring the compositor up, build PSRAM
//  layer caches once, and per frame submit FILL/BLIT/BLEND non-blocking,
//  serialising the overlapping ops with se_ppa_wait_one() (PPA does not
//  order ops across client types, so overlapping writes need a barrier).
//
//  Illustrative, not part of this game's build. ESP32-P4 only (PPA).
// =====================================================================

#include <math.h>

#include "synthengine3d.h"   // the whole public API (incl. se_ppa.h)

// Logical band layout (logical = post-orientation screen coordinates).
#define SKY_TOP       0
#define SKY_H         200       // sky band height
#define SUN_H         120       // sun layer height
#define HILLS_TOP     150       // where the hills sit
#define HILLS_H       120

#define SKY_ARGB      0xFF552075u   // purple
#define HILLS_KEY     0xFF00FF00u   // pure green -> keyed transparent
#define HILLS_CK_LO   0x0000FC00u   // key window on the expanded RGB888 fg
#define HILLS_CK_HI   0x0000FF00u

static se_ppa_layer_t s_sun   = {0};
static se_ppa_layer_t s_hills = {0};
static int            s_log_w  = 0;   // display logical width
static float          s_t      = 0.0f;

// The framebuffer is stored raw (pre-orientation); logical dims are the
// transpose under a quarter-turn. Derive logical width/height from the
// engine's resolved display info.
static void logical_dims(se_display_info_t const* di, int* lw, int* lh) {
    bool const quarter = (di->orientation == PAX_O_ROT_CW ||
                          di->orientation == PAX_O_ROT_CCW);
    *lw = quarter ? (int)di->height : (int)di->width;
    *lh = quarter ? (int)di->width  : (int)di->height;
}

static void on_init(void* user) {
    (void)user;
    se_ppa_init();

    se_display_info_t di;
    se_display_info(&di);
    int lw, lh;
    logical_dims(&di, &lw, &lh);
    s_log_w = lw;

    // Build the two layer caches once, matching the framebuffer format /
    // orientation, then draw artwork into each and flush it for the PPA DMA.
    se_ppa_layer_alloc(&s_sun,   lw, SUN_H,   di.pax_format, di.reversed, di.orientation);
    se_ppa_layer_alloc(&s_hills, lw, HILLS_H, di.pax_format, di.reversed, di.orientation);

    // Sun: three horizontal bands (a classic synthwave sun gradient).
    pax_background(&s_sun.buf, 0xFFFF8030u);
    pax_draw_rect(&s_sun.buf, 0xFFFF40A0u, 0, 0,            (float)lw, SUN_H / 3.0f);
    pax_draw_rect(&s_sun.buf, 0xFFFFC040u, 0, SUN_H * 2/3.0f, (float)lw, SUN_H / 3.0f);
    se_ppa_layer_flush(&s_sun);

    // Hills: green-keyed background (shows the sky/sun through) + a magenta
    // silhouette. The green never survives the colour-key, so only the
    // silhouette composites over the framebuffer.
    pax_background(&s_hills.buf, HILLS_KEY);
    pax_draw_tri(&s_hills.buf, 0xFFB02080u, 0, HILLS_H, lw * 0.35f, 20, lw * 0.7f, HILLS_H);
    se_ppa_layer_flush(&s_hills);
}

static void on_update(float dt, void* user) {
    (void)user;
    s_t += dt;
}

// The backdrop hook: composite sky -> sun -> hills on the PPA. Each op is a
// non-blocking submit; the wait_one() barriers pin the order because all
// three touch the same upper region on different client types.
static void on_backdrop(pax_buf_t* fb, void* user) {
    (void)user;

    se_ppa_fill(fb, SKY_TOP, SKY_H, SKY_ARGB);
    se_ppa_wait_one();

    // Slide the sun vertically (a setting-sun bob): top y in [40, 120].
    int const sun_y = 80 + (int)(40.0f * sinf(s_t));
    se_ppa_blit(fb, &s_sun, sun_y);
    se_ppa_wait_one();

    se_ppa_blend_key(fb, &s_hills, HILLS_TOP, HILLS_CK_LO, HILLS_CK_HI);
    se_ppa_wait_one();   // backdrop must be in place before on_render draws
}

static void on_render(pax_buf_t* fb, void* user) {
    (void)user;
    // Foreground 3D scene would go here (see the minimal example). The PPA
    // backdrop is already composited; the CPU drew nothing for it.
    (void)fb;
}

void app_main(void) {
    static se_app_config_t const cfg = {
        .f1_exits = true,
        // backdrop_argb is unused here: we register an on_backdrop hook, so
        // the engine calls it instead of clearing to a flat colour.
    };
    static se_app_callbacks_t const cb = {
        .on_init     = on_init,
        .on_update   = on_update,
        .on_backdrop = on_backdrop,
        .on_render   = on_render,
    };
    se_run(&cfg, &cb, NULL);
}
