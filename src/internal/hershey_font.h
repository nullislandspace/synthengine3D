// Hershey Vector Font - PAX graphics rendering
// Based on the Hershey Simplex font from paulbourke.net/dataformats/hershey/
// Public domain
// Adapted for pax_buf_t rendering
//
// The fallback path: USE_HERSHEY_DIRECT off. Strings are UTF-8 and the
// glyphs come from hershey_text.h, exactly as in hershey_font_direct.h,
// so the two paths draw the same text at the same widths.

#ifndef HERSHEY_FONT_H
#define HERSHEY_FONT_H

#include <stdlib.h>
#include "pax_gfx.h"
#include "hershey.h"
#include "hershey_text.h"

// Font metrics
#define HERSHEY_BASE_HEIGHT 21  // Capital letter height in font units

// One run of strokes, in font units, offset by (fx, fy) font units from
// the pen. Two flavours, for the two widths a stored coordinate has.
#define HERSHEY_PAX_STROKES(TYPE, NAME)                                                       \
    static inline void NAME(pax_buf_t* buf, pax_col_t color, float screen_x, float screen_y,  \
                            float scale, TYPE const* p, int n, int fx, int fy) {              \
        int   pen_down = 0;                                                                   \
        float prev_sx = 0, prev_sy = 0;                                                       \
        for (int i = 0; i < n; i++) {                                                         \
            int const vx = (int)p[i * 2];                                                     \
            int const vy = (int)p[i * 2 + 1];                                                 \
            if (vx == -1 && vy == -1) {                                                       \
                pen_down = 0;                                                                 \
                continue;                                                                     \
            }                                                                                 \
            /* Font Y goes up (0=baseline, 21=cap), flip for screen Y (down) */               \
            float const sx = screen_x + (vx + fx) * scale;                                    \
            float const sy = screen_y + (HERSHEY_BASE_HEIGHT - (vy + fy)) * scale;            \
            if (pen_down) pax_simple_line(buf, color, prev_sx, prev_sy, sx, sy);              \
            prev_sx  = sx;                                                                    \
            prev_sy  = sy;                                                                    \
            pen_down = 1;                                                                     \
        }                                                                                     \
    }

HERSHEY_PAX_STROKES(int, hershey_pax_strokes32)
HERSHEY_PAX_STROKES(int8_t, hershey_pax_strokes8)

// Draw a single character from the Hershey font, by codepoint
// screen_x, screen_y: screen coordinates (y=0 at top)
// font_height: desired font height in pixels
// Returns: scaled character width for horizontal advance
static inline int hershey_draw_cp(pax_buf_t *buf, pax_col_t color,
                                  float screen_x, float screen_y, uint32_t cp, float font_height) {
    float const     scale = font_height / HERSHEY_BASE_HEIGHT;
    hershey_glyph_t g;
    if (!hershey_glyph(cp, &g)) {
        static int8_t const tofu[] = {2, 0, 2, 14, 10, 14, 10, 0, 2, 0};
        hershey_pax_strokes8(buf, color, screen_x, screen_y, scale, tofu, 5, 0, 0);
        return (int)(HERSHEY_TOFU_ADV * scale);
    }
    if (g.n > 0) {
        if (g.pts32 != NULL) {
            hershey_pax_strokes32(buf, color, screen_x, screen_y, scale, g.pts32, g.n, 0, 0);
        } else {
            hershey_pax_strokes8(buf, color, screen_x, screen_y, scale, g.pts8, g.n, 0, 0);
        }
    }
    if (g.accent != SE_ACCENT_NONE) {
        if (g.accent == SE_ACCENT_SLASH) {
            int const    top    = (g.accent_y > HERSHEY_X_H + 2) ? HERSHEY_CAP_H : HERSHEY_X_H + 2;
            int8_t const bar[4] = {0, -2, (int8_t)g.adv, (int8_t)top};
            hershey_pax_strokes8(buf, color, screen_x, screen_y, scale, bar, 2, 0, 0);
        } else {
            hershey_pax_strokes8(buf, color, screen_x, screen_y, scale,
                                 &SE_HERSHEY_ACCENT_PTS[2 * SE_HERSHEY_ACCENT[g.accent].off],
                                 SE_HERSHEY_ACCENT[g.accent].n, g.accent_x, g.accent_y);
        }
    }
    return (int)(g.adv * scale);
}

// Draw a UTF-8 string using the Hershey font
// screen_x, screen_y: screen position (top-left of first character)
// font_height: desired font height in pixels
// Returns: pax_vec2f with total width and font height
static inline pax_vec2f hershey_draw_string(pax_buf_t *buf, pax_col_t color,
                                            float screen_x, float screen_y, const char *str, float font_height) {
    float start_x = screen_x;
    for (;;) {
        uint32_t  cp;
        int const used = hershey_utf8_next(str, &cp);
        if (used == 0) break;
        str += used;
        if (cp != 0) screen_x += hershey_draw_cp(buf, color, screen_x, screen_y, cp, font_height);
    }
    return (pax_vec2f){screen_x - start_x, font_height};
}

// Calculate the width of a string without drawing it
// font_height: desired font height in pixels
static inline pax_vec2f hershey_string_size(float font_height, const char *str) {
    float scale = font_height / HERSHEY_BASE_HEIGHT;
    int   width = 0;
    for (;;) {
        uint32_t  cp;
        int const used = hershey_utf8_next(str, &cp);
        if (used == 0) break;
        str += used;
        if (cp != 0) width += (int)(hershey_advance(cp) * scale);
    }
    return (pax_vec2f){(float)width, font_height};
}

#endif // HERSHEY_FONT_H
