// Hershey Vector Font — direct pixel rendering into a pax_buf_t.
// Built on top of the shared `se_direct565.h` helpers, so the inner
// per-pixel loop is identical to the one used by `synthwave_step`
// and `render_obstacles`: one halfword store, no orientation
// switch, no PAX setter dispatch.
//
// Based on the Hershey Simplex font from paulbourke.net/dataformats/hershey/
// Public domain.

#ifndef HERSHEY_FONT_DIRECT_H
#define HERSHEY_FONT_DIRECT_H

#include <stdlib.h>

#include "se_direct565.h"
#include "hershey.h"
#include "pax_gfx.h"

// Font metrics
#define HERSHEY_DIRECT_BASE_HEIGHT 21

// Draw a single character. The framebuffer's raw pixel pointer and
// the pre-packed colour are passed in so per-character setup work
// is reduced to two arithmetic ops at the call site. Returns the
// horizontal advance for the caller's pen.
static inline int hershey_direct_draw_char(uint16_t* pixels, uint16_t packed,
                                           float screen_x, float screen_y, char ch, float font_height) {
    float scale = font_height / HERSHEY_DIRECT_BASE_HEIGHT;
    int idx = (int)ch - 32;
    if (idx < 0 || idx >= 95) {
        return (int)(16 * scale);
    }

    const int *glyph = simplex[idx];
    int num_vertices = glyph[0];
    int char_width = glyph[1];

    if (num_vertices == 0) {
        return (int)(char_width * scale);
    }

    int pen_down = 0;
    int prev_sx = 0, prev_sy = 0;

    for (int i = 0; i < num_vertices; i++) {
        int vx = glyph[2 + i * 2];
        int vy = glyph[2 + i * 2 + 1];

        if (vx == -1 && vy == -1) {
            pen_down = 0;
            continue;
        }

        int gx = (int)(vx * scale);
        int gy = (int)((HERSHEY_DIRECT_BASE_HEIGHT - vy) * scale);

        int sx = (int)screen_x + gx;
        int sy = (int)screen_y + gy;

        if (pen_down) {
            direct_565_line(pixels, prev_sx, prev_sy, sx, sy, packed);
        }

        prev_sx = sx;
        prev_sy = sy;
        pen_down = 1;
    }

    return (int)(char_width * scale);
}

// Draw a NUL-terminated string. Packs the colour once for the
// whole string and extracts the raw framebuffer pointer once;
// inside the per-char loop the inner Bresenham works directly on
// raw pixels.
static inline pax_vec2f hershey_direct_draw_string(pax_buf_t *buf, pax_col_t color,
                                                   float screen_x, float screen_y,
                                                   const char *str, float font_height) {
    uint16_t  const packed = direct_565_pack_for(buf, color);
    uint16_t* const pixels = (uint16_t*)pax_buf_get_pixels(buf);

    float start_x = screen_x;
    while (*str) {
        screen_x += hershey_direct_draw_char(pixels, packed, screen_x, screen_y, *str, font_height);
        str++;
    }
    return (pax_vec2f){screen_x - start_x, font_height};
}

// Calculate string width without drawing.
static inline pax_vec2f hershey_direct_string_size(float font_height, const char *str) {
    float scale = font_height / HERSHEY_DIRECT_BASE_HEIGHT;
    int width = 0;
    while (*str) {
        int idx = (int)*str - 32;
        if (idx >= 0 && idx < 95) {
            width += (int)(simplex[idx][1] * scale);
        } else {
            width += (int)(16 * scale);
        }
        str++;
    }
    return (pax_vec2f){(float)width, font_height};
}

#endif // HERSHEY_FONT_DIRECT_H
