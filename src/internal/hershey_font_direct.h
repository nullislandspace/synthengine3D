// Hershey Vector Font — direct pixel rendering into a pax_buf_t.
// Built on top of the shared `se_direct565.h` helpers, so the inner
// per-pixel loop is identical to the one used by `synthwave_step`
// and `render_obstacles`: one halfword store, no orientation
// switch, no PAX setter dispatch.
//
// Based on the Hershey Simplex font from paulbourke.net/dataformats/hershey/
// Public domain.
//
// Strings are UTF-8. Which glyph a codepoint draws as -- ASCII from
// `simplex`, Cyrillic and the rest from `hershey_ext.h`, accented Latin
// as a letter plus an accent -- is hershey_text.h's business, and this
// file only draws what it is handed.

#ifndef HERSHEY_FONT_DIRECT_H
#define HERSHEY_FONT_DIRECT_H

#include <stdlib.h>

#include "se_direct565.h"
#include "hershey.h"
#include "hershey_text.h"
#include "pax_gfx.h"

// Font metrics
#define HERSHEY_DIRECT_BASE_HEIGHT 21

// One run of strokes, in font units, offset by (fx, fy) font units from
// the pen. The two flavours differ only in how wide a stored coordinate
// is: `simplex` keeps ints, the ext glyphs int8.
#define HERSHEY_DIRECT_STROKES(TYPE, NAME)                                                       \
    static inline void NAME(uint16_t* pixels, uint16_t packed, float screen_x, float screen_y,   \
                            float scale, TYPE const* p, int n, int fx, int fy) {                 \
        int pen_down = 0;                                                                        \
        int prev_sx = 0, prev_sy = 0;                                                            \
        for (int i = 0; i < n; i++) {                                                            \
            int const vx = (int)p[i * 2];                                                        \
            int const vy = (int)p[i * 2 + 1];                                                    \
            if (vx == -1 && vy == -1) {                                                          \
                pen_down = 0;                                                                    \
                continue;                                                                        \
            }                                                                                    \
            int const sx = (int)screen_x + (int)((vx + fx) * scale);                             \
            int const sy = (int)screen_y + (int)((HERSHEY_DIRECT_BASE_HEIGHT - (vy + fy)) * scale); \
            if (pen_down) direct_565_line(pixels, prev_sx, prev_sy, sx, sy, packed);             \
            prev_sx  = sx;                                                                       \
            prev_sy  = sy;                                                                       \
            pen_down = 1;                                                                        \
        }                                                                                        \
    }

HERSHEY_DIRECT_STROKES(int, hershey_direct_strokes32)
HERSHEY_DIRECT_STROKES(int8_t, hershey_direct_strokes8)

// Draw a single character, by codepoint. The framebuffer's raw pixel
// pointer and the pre-packed colour are passed in so per-character setup
// work is reduced to two arithmetic ops at the call site. Returns the
// horizontal advance for the caller's pen.
static inline int hershey_direct_draw_cp(uint16_t* pixels, uint16_t packed,
                                         float screen_x, float screen_y, uint32_t cp, float font_height) {
    float const     scale = font_height / HERSHEY_DIRECT_BASE_HEIGHT;
    hershey_glyph_t g;
    if (!hershey_glyph(cp, &g)) {
        // No glyph: an empty box, so a translation using a letter this
        // font has never heard of says so instead of going quiet.
        static int8_t const tofu[] = {2, 0, 2, 14, 10, 14, 10, 0, 2, 0};
        hershey_direct_strokes8(pixels, packed, screen_x, screen_y, scale, tofu, 5, 0, 0);
        return (int)(HERSHEY_TOFU_ADV * scale);
    }
    if (g.n > 0) {
        if (g.pts32 != NULL) {
            hershey_direct_strokes32(pixels, packed, screen_x, screen_y, scale, g.pts32, g.n, 0, 0);
        } else {
            hershey_direct_strokes8(pixels, packed, screen_x, screen_y, scale, g.pts8, g.n, 0, 0);
        }
    }
    if (g.accent != SE_ACCENT_NONE) {
        if (g.accent == SE_ACCENT_SLASH) {
            // A bar through the letter, corner to corner (ø, Ø).
            int const top = (g.accent_y > HERSHEY_X_H + 2) ? HERSHEY_CAP_H : HERSHEY_X_H + 2;
            int8_t const bar[4] = {0, -2, (int8_t)g.adv, (int8_t)top};
            hershey_direct_strokes8(pixels, packed, screen_x, screen_y, scale, bar, 2, 0, 0);
        } else {
            hershey_direct_strokes8(pixels, packed, screen_x, screen_y, scale,
                                    &SE_HERSHEY_ACCENT_PTS[2 * SE_HERSHEY_ACCENT[g.accent].off],
                                    SE_HERSHEY_ACCENT[g.accent].n, g.accent_x, g.accent_y);
        }
    }
    return (int)(g.adv * scale);
}

// Draw a NUL-terminated UTF-8 string. Packs the colour once for the
// whole string and extracts the raw framebuffer pointer once; inside
// the per-char loop the inner Bresenham works directly on raw pixels.
static inline pax_vec2f hershey_direct_draw_string(pax_buf_t *buf, pax_col_t color,
                                                   float screen_x, float screen_y,
                                                   const char *str, float font_height) {
    uint16_t  const packed = direct_565_pack_for(buf, color);
    uint16_t* const pixels = (uint16_t*)pax_buf_get_pixels(buf);

    float start_x = screen_x;
    for (;;) {
        uint32_t  cp;
        int const used = hershey_utf8_next(str, &cp);
        if (used == 0) break;
        str += used;
        if (cp != 0) screen_x += hershey_direct_draw_cp(pixels, packed, screen_x, screen_y, cp, font_height);
    }
    return (pax_vec2f){screen_x - start_x, font_height};
}

// Calculate string width without drawing.
static inline pax_vec2f hershey_direct_string_size(float font_height, const char *str) {
    float scale = font_height / HERSHEY_DIRECT_BASE_HEIGHT;
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

#endif // HERSHEY_FONT_DIRECT_H
