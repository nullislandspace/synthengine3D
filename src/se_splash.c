// =====================================================================
//  SynthEngine3D  --  engine splash screen
// ---------------------------------------------------------------------
//  Public API + contract: include/se_splash.h.
//
//  The wordmark is real geometry, not a scaled image, and the two lines
//  are built differently on purpose:
//
//    title     SOLID BLOCKS. Each Hershey stroke is walked at a fixed arc
//              length and a small extruded cube is dropped at every step,
//              so the letters are made of blocks with a shallow depth
//              rather than drawn as outlines. Hershey is a STROKE font --
//              there are no outlines to fill -- so marching along the
//              strokes is what turns it into solid 3D lettering.
//    subtitle  vector strokes via scene_line(), which is all a line of
//              small text needs.
//
//  The animation flies that geometry from far to near through the
//  engine's own pinhole camera, then holds. So the "zoom" is an actual
//  perspective approach -- the wordmark grows AND spreads outward from
//  the vanishing point, and the blocks' side faces swing into view at the
//  edges the way real geometry does -- which a 2D blit stretch cannot
//  reproduce.
//
//  Each block emits only its three potentially-visible faces (front, plus
//  whichever side and top/bottom faces turn toward the eye), decided from
//  the block's position relative to the camera axis. The engine has no
//  back-face culling by design -- se_scene.h says to cull at emit time,
//  where the emitter knows its own normals -- so this is the sanctioned
//  place to do it, and it halves the triangle count.
// =====================================================================

#include "se_splash.h"

#include <stdbool.h>
#include <stdint.h>
#include <math.h>
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

// How long the wordmark sits still after the zoom finishes, in seconds.
// The zoom duration is the caller's `seconds`; this is added on top.
#define SPLASH_HOLD_SECONDS 2.0f

// Block geometry for the title, in font units (cap height is 21, so a
// half-extent of 1.6 puts roughly six blocks across a capital letter).
// SPLASH_BLOCK_STEP is the arc length between blocks along a stroke, set
// slightly under the block width so consecutive blocks overlap and the
// strokes read as continuous instead of dotted.
#define SPLASH_BLOCK_HALF   1.6f
#define SPLASH_BLOCK_STEP   2.6f
#define SPLASH_BLOCK_DEPTH  1.6f

// Upper bound on blocks per title. Each block is 6 triangles, so this
// keeps the title inside SCENE_TRI_CAP (4096) with headroom. The default
// wording needs ~243; a long custom title passed to se_splash_ex() would
// otherwise overrun the cap and have geometry silently dropped, so the
// step is widened to fit instead of letting letters come out broken.
#define SPLASH_MAX_BLOCKS   560

// Face shading, as a multiplier on the title colour. The front face takes
// the full brightness; the side and top/bottom faces are darker, which is
// what makes the blocks read as solid rather than flat.
#define SPLASH_FACE_SIDE    0.50f
#define SPLASH_FACE_TOP     0.76f

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

// Total stroke path length of a string, in font units -- what the block
// marcher will walk. Used to size the step so the block count stays inside
// the triangle budget.
static float splash_path_units(char const* s) {
    float total = 0.0f;
    for (; *s; s++) {
        int const gi = (int)(unsigned char)*s - 32;
        if (gi < 0 || gi >= 95) continue;
        int const n = simplex[gi][0];
        float px = 0.0f, py = 0.0f;
        bool  down = false;
        for (int i = 0; i < n; i++) {
            int const vx = simplex[gi][2 + i * 2];
            int const vy = simplex[gi][2 + i * 2 + 1];
            if (vx == -1 && vy == -1) { down = false; continue; }
            if (down) {
                float const dx = (float)vx - px, dy = (float)vy - py;
                total += sqrtf(dx * dx + dy * dy);
            }
            px = (float)vx; py = (float)vy; down = true;
        }
    }
    return total;
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

// One block of the title: an axis-aligned box centred at (cx, cy) on the
// plane z, extruded +/- `depth` about it.
//
// Only the three faces that can face the eye are emitted. The camera sits
// at the origin looking down +z, so: the near face (at z - depth) always
// faces it; of the two x-faces the visible one is the one turned back
// toward the camera axis (a block left of x = 0 shows its RIGHT face); and
// likewise a block above eye level shows its BOTTOM face. Emitting the
// other three would be invisible overdraw -- the engine has no back-face
// culling, by design (se_scene.h: cull where the normals are known).
static void splash_block(float cx, float cy, float z, float half, float depth,
                         uint32_t front, uint32_t side, uint32_t top) {
    float const x0 = cx - half, x1 = cx + half;
    float const y0 = cy - half, y1 = cy + half;
    float const zf = z - depth, zb = z + depth;   // zf is the nearer face

    // Near face -- always toward the eye.
    scene_tri(x0, y0, zf,  x1, y0, zf,  x1, y1, zf, front);
    scene_tri(x0, y0, zf,  x1, y1, zf,  x0, y1, zf, front);

    // The x-face turned back toward the camera axis.
    float const sx = (cx < 0.0f) ? x1 : x0;
    scene_tri(sx, y0, zf,  sx, y1, zf,  sx, y1, zb, side);
    scene_tri(sx, y0, zf,  sx, y1, zb,  sx, y0, zb, side);

    // Above the eye we see the underside; below it, the top.
    float const sy = (cy > 0.0f) ? y0 : y1;
    scene_tri(x0, sy, zf,  x1, sy, zf,  x1, sy, zb, top);
    scene_tri(x0, sy, zf,  x1, sy, zb,  x0, sy, zb, top);
}

// Emit a string as solid blocks: march every stroke at a constant arc
// length, dropping a block at each step. Leftover distance carries into
// the next segment (`carry`) so spacing stays even across corners instead
// of restarting -- otherwise blocks would bunch up at every vertex.
static void splash_emit_blocks(char const* s, float baseline_y, float z,
                               float scale, float step, uint32_t argb, float k) {
    uint32_t const front = splash_shade(argb, k);
    uint32_t const side  = splash_shade(argb, k * SPLASH_FACE_SIDE);
    uint32_t const top   = splash_shade(argb, k * SPLASH_FACE_TOP);
    float const half  = SPLASH_BLOCK_HALF  * scale;
    float const depth = SPLASH_BLOCK_DEPTH * scale;
    if (step <= 0.0f) return;

    float pen = -splash_width_units(s) * scale * 0.5f;
    for (; *s; s++) {
        int const gi = (int)(unsigned char)*s - 32;
        if (gi < 0 || gi >= 95) { pen += SPLASH_FALLBACK_ADV * scale; continue; }

        int const n = simplex[gi][0];
        float px = 0.0f, py = 0.0f, carry = 0.0f;
        bool  down = false;
        for (int i = 0; i < n; i++) {
            int const vx = simplex[gi][2 + i * 2];
            int const vy = simplex[gi][2 + i * 2 + 1];
            if (vx == -1 && vy == -1) { down = false; continue; }   // pen up
            float const wx = pen        + (float)vx * scale;
            float const wy = baseline_y + (float)vy * scale;

            if (!down) {
                // Start of a stroke: anchor a block on the first point.
                splash_block(wx, wy, z, half, depth, front, side, top);
                carry = step;
            } else {
                float const dx = wx - px, dy = wy - py;
                float const len = sqrtf(dx * dx + dy * dy);
                if (len > 1e-6f) {
                    float t = carry;
                    while (t <= len) {
                        float const u = t / len;
                        splash_block(px + dx * u, py + dy * u, z, half, depth,
                                     front, side, top);
                        t += step;
                    }
                    carry = t - len;   // distance owed to the next segment
                }
            }
            px = wx; py = wy; down = true;
        }
        pen += (float)simplex[gi][1] * scale;
    }
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

    // Block spacing: the nominal step, widened if this title would need
    // more blocks than the triangle budget allows (a long custom title).
    // Wider blocks look coarser but never lose geometry.
    float       block_step = SPLASH_BLOCK_STEP * scale;
    float const path       = splash_path_units(title) * scale;
    if (path > 0.0f && (path / block_step) > (float)SPLASH_MAX_BLOCKS) {
        block_step = path / (float)SPLASH_MAX_BLOCKS;
    }

    int64_t const t0       = esp_timer_get_time();
    float   const zoom_us  = seconds * 1000000.0f;
    float   const total_us = zoom_us + SPLASH_HOLD_SECONDS * 1000000.0f;
    for (;;) {
        float const el = (float)(esp_timer_get_time() - t0);
        // Zoom progress saturates at 1, so the frames after it are the
        // finished wordmark held still until total_us elapses.
        float const t = (el < zoom_us) ? (el / zoom_us) : 1.0f;
        // Ease out: fast approach that decelerates into place, so it
        // settles rather than slamming to a stop.
        float const e = 1.0f - (1.0f - t) * (1.0f - t);
        float const z = SPLASH_Z_FAR + (SPLASH_Z_NEAR - SPLASH_Z_FAR) * e;
        float const k = 0.30f + 0.70f * e;   // brightness ramp

        // Re-read the back buffer every frame: present() swaps it. The held
        // frames still have to be redrawn -- with double buffering the two
        // buffers alternate, so skipping the redraw would flicker.
        pax_buf_t* const fb = se_frame_back();
        pax_background(fb, 0xFF000000u);

        render_set_camera_6dof(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        scene_begin(fb);
        splash_emit_blocks(title, SPLASH_TITLE_BASE * cap, z, scale, block_step,
                           SPLASH_TITLE_ARGB, k);
        splash_emit(subtitle, SPLASH_SUB_BASE * cap, z, scale * SPLASH_SUB_SCALE,
                    splash_shade(SPLASH_SUB_ARGB, k));
        scene_render(SE_RENDER_DEFAULT);

        se_frame_present();
        if (el >= total_us) break;
    }
}

void se_splash(void) {
    se_splash_ex(NULL, NULL, 0.0f);
}
