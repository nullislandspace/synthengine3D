// =====================================================================
//  SynthEngine3D  --  engine splash screen
// ---------------------------------------------------------------------
//  Public API + contract: include/se_splash.h.
//
//  The wordmark is real geometry, not a scaled image. Each glyph of the
//  Hershey simplex font is walked stroke by stroke and emitted as
//  world-space scene_line() segments on a single z plane; the animation
//  then flies that plane from far to near through the engine's own
//  pinhole camera. So the "zoom" is an actual perspective approach --
//  the text grows AND spreads outward from the vanishing point the way
//  approaching geometry does, which a 2D blit stretch cannot reproduce.
//
//  Cost is trivial: ~700 line segments for the default wording, against
//  a 4096 edge cap, and the depth buffer never fills because lines test
//  depth without writing it.
// =====================================================================

#include "se_splash.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "pax_gfx.h"
#include "se_config.h"    // DISPLAY_LOG_W, RENDER_FOCAL_LEN
#include "se_frame.h"     // internal: borrow se_run's framebuffer + present
#include "se_scene.h"     // scene_begin / scene_line / scene_render / camera
#include "se_text.h"      // simplex[][] glyph table
#include "se_version.h"   // se_version_string (default subtitle)

static char const TAG[] = "se_splash";

// Hershey capital-letter height, in font units (0 = baseline, 21 = cap).
// Font Y already points up, which is also world +y, so glyph vertices go
// straight into world space with no flip -- unlike the 2D text path,
// which has to invert for screen coordinates.
#define SPLASH_CAP_UNITS    21.0f

// The flight path, in world units along +z. Ends far enough out that the
// wordmark still fits the screen, starts far enough back that the
// approach reads as motion rather than a pop.
#define SPLASH_Z_FAR        9.0f
#define SPLASH_Z_NEAR       2.2f

// Title width at the end of the zoom, as a fraction of screen width.
#define SPLASH_TITLE_FRAC   0.72f
// Subtitle size relative to the title.
#define SPLASH_SUB_SCALE    0.42f

// Baselines relative to cap height, measured from the zoom's centre (world
// y = 0) so the whole block expands about the vanishing point.
#define SPLASH_TITLE_BASE   0.25f
#define SPLASH_SUB_BASE    (-0.95f)

#define SPLASH_TITLE_ARGB   0xFF31FBFBu   // cyan
#define SPLASH_SUB_ARGB     0xFFF71FF1u   // magenta

// Advance used for a character outside the glyph table, matching the 2D
// text path's fallback so measurement and drawing agree.
#define SPLASH_FALLBACK_ADV 16.0f

// String width in font units (the sum of the glyph advances).
static float splash_width_units(char const* s) {
    float w = 0.0f;
    for (; *s; s++) {
        int const gi = (int)(unsigned char)*s - 32;
        w += (gi >= 0 && gi < 95) ? (float)simplex[gi][1] : SPLASH_FALLBACK_ADV;
    }
    return w;
}

// Emit one string as world-space line segments on the plane z, centred on
// x = 0, sitting on the given baseline. `scale` converts font units to
// world units.
static void splash_emit(char const* s, float baseline_y, float z,
                        float scale, uint32_t argb) {
    float pen = -splash_width_units(s) * scale * 0.5f;
    for (; *s; s++) {
        int const gi = (int)(unsigned char)*s - 32;
        if (gi < 0 || gi >= 95) { pen += SPLASH_FALLBACK_ADV * scale; continue; }

        int const n = simplex[gi][0];   // vertex count (0 for space)
        float px = 0.0f, py = 0.0f;
        bool  down = false;
        for (int i = 0; i < n; i++) {
            int const vx = simplex[gi][2 + i * 2];
            int const vy = simplex[gi][2 + i * 2 + 1];
            if (vx == -1 && vy == -1) { down = false; continue; }   // pen up
            float const wx = pen        + (float)vx * scale;
            float const wy = baseline_y + (float)vy * scale;
            if (down) scene_line(px, py, z, wx, wy, z, argb);
            px = wx; py = wy; down = true;
        }
        pen += (float)simplex[gi][1] * scale;
    }
}

// Scale an ARGB colour's RGB by k (0..1), keeping it opaque. Used to ramp
// the wordmark up out of the dark as it approaches -- the renderer has no
// alpha blending (it writes packed 565), so brightness is the fade.
static uint32_t splash_shade(uint32_t argb, float k) {
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;
    uint32_t const r = (uint32_t)((float)((argb >> 16) & 0xFFu) * k);
    uint32_t const g = (uint32_t)((float)((argb >>  8) & 0xFFu) * k);
    uint32_t const b = (uint32_t)((float)( argb        & 0xFFu) * k);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void se_splash_ex(char const* title, char const* subtitle, float seconds) {
    if (se_frame_back() == NULL) {
        ESP_LOGW(TAG, "se_splash before se_run() bootstrap -- skipped");
        return;
    }

    char subbuf[40];
    if (title == NULL) title = "SynthEngine 3D";
    if (subtitle == NULL) {
        snprintf(subbuf, sizeof subbuf, "Version %s", se_version_string());
        subtitle = subbuf;
    }
    if (seconds <= 0.0f) seconds = 1.0f;

    // Pick the world scale from the on-screen size we want at the END of
    // the flight: a world width W at depth z spans W * FOCAL / z pixels,
    // so invert that at z = SPLASH_Z_NEAR. Doing it this way keeps the
    // wordmark correctly sized if the display or FOV constants change.
    float const tw_units = splash_width_units(title);
    if (tw_units <= 0.0f) return;
    float const target_px = SPLASH_TITLE_FRAC * (float)DISPLAY_LOG_W;
    float const scale     = (target_px * SPLASH_Z_NEAR / RENDER_FOCAL_LEN) / tw_units;
    float const cap       = SPLASH_CAP_UNITS * scale;

    int64_t const t0 = esp_timer_get_time();
    for (;;) {
        float t = (float)(esp_timer_get_time() - t0) / (seconds * 1000000.0f);
        if (t > 1.0f) t = 1.0f;
        // Ease out: fast approach that decelerates into place, so it
        // settles rather than slamming to a stop.
        float const e = 1.0f - (1.0f - t) * (1.0f - t);
        float const z = SPLASH_Z_FAR + (SPLASH_Z_NEAR - SPLASH_Z_FAR) * e;
        float const k = 0.30f + 0.70f * e;   // brightness ramp

        // Re-read the back buffer every frame: present() swaps it.
        pax_buf_t* const fb = se_frame_back();
        pax_background(fb, 0xFF000000u);

        render_set_camera_6dof(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        scene_begin(fb);
        splash_emit(title,    SPLASH_TITLE_BASE * cap, z, scale,
                    splash_shade(SPLASH_TITLE_ARGB, k));
        splash_emit(subtitle, SPLASH_SUB_BASE * cap, z, scale * SPLASH_SUB_SCALE,
                    splash_shade(SPLASH_SUB_ARGB, k));
        scene_render(SE_RENDER_DEFAULT);

        se_frame_present();
        if (t >= 1.0f) break;
    }
}

void se_splash(void) {
    se_splash_ex(NULL, NULL, 0.0f);
}
