// =====================================================================
//  textsheet -- draw UTF-8 lines with the engine's own glyph tables
// ---------------------------------------------------------------------
//  The font is strokes in a header, so the only way to know what a
//  translation will look like on the badge is to draw it. This builds on
//  the host, includes the same hershey_text.h the renderer does, and
//  writes a PGM.
//
//      cc -I../../src/internal -o textsheet textsheet.c -lm
//      ./textsheet 28 "Grüße" "Български" > sheet.pgm
//
//  First argument is the text height in pixels; the rest are lines. A
//  codepoint with no glyph comes out as an empty box, exactly as it
//  would in the game.
// =====================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hershey_text.h"

#define W 1600
#define H 1200

static unsigned char img[H][W];

static void plot(int x, int y) {
    if (x >= 0 && x < W && y >= 0 && y < H) img[y][x] = 0;
}

static void line(float x0, float y0, float x1, float y1) {
    float const dx = x1 - x0, dy = y1 - y0;
    int const   n  = (int)(((dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy)) + 1);
    for (int i = 0; i <= n; i++) {
        float const t = (float)i / (float)n;
        plot((int)(x0 + dx * t + 0.5f), (int)(y0 + dy * t + 0.5f));
    }
}

#define STROKES(TYPE, NAME)                                                                  \
    static void NAME(float ox, float oy, float scale, TYPE const* p, int n, int fx, int fy) { \
        int   pen = 0;                                                                       \
        float px = 0, py = 0;                                                                \
        for (int i = 0; i < n; i++) {                                                        \
            int const vx = (int)p[i * 2], vy = (int)p[i * 2 + 1];                            \
            if (vx == -1 && vy == -1) {                                                      \
                pen = 0;                                                                     \
                continue;                                                                    \
            }                                                                                \
            float const sx = ox + (vx + fx) * scale;                                         \
            float const sy = oy + (HERSHEY_CAP_H - (vy + fy)) * scale;                       \
            if (pen) line(px, py, sx, sy);                                                   \
            px  = sx;                                                                        \
            py  = sy;                                                                        \
            pen = 1;                                                                         \
        }                                                                                    \
    }

STROKES(int, strokes32)
STROKES(int8_t, strokes8)

static float draw_cp(float x, float y, uint32_t cp, float height) {
    float const     scale = height / HERSHEY_CAP_H;
    hershey_glyph_t g;
    if (!hershey_glyph(cp, &g)) {
        static int8_t const tofu[] = {2, 0, 2, 14, 10, 14, 10, 0, 2, 0};
        strokes8(x, y, scale, tofu, 5, 0, 0);
        return HERSHEY_TOFU_ADV * scale;
    }
    if (g.n > 0) {
        if (g.pts32) strokes32(x, y, scale, g.pts32, g.n, 0, 0);
        else         strokes8(x, y, scale, g.pts8, g.n, 0, 0);
    }
    if (g.accent != SE_ACCENT_NONE) {
        if (g.accent == SE_ACCENT_SLASH) {
            int const    top    = (g.accent_y > HERSHEY_X_H + 2) ? HERSHEY_CAP_H : HERSHEY_X_H + 2;
            int8_t const bar[4] = {0, -2, (int8_t)g.adv, (int8_t)top};
            strokes8(x, y, scale, bar, 2, 0, 0);
        } else {
            strokes8(x, y, scale, &SE_HERSHEY_ACCENT_PTS[2 * SE_HERSHEY_ACCENT[g.accent].off],
                     SE_HERSHEY_ACCENT[g.accent].n, g.accent_x, g.accent_y);
        }
    }
    return g.adv * scale;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <height-px> <line> [line ...] > sheet.pgm\n", argv[0]);
        return 2;
    }
    memset(img, 0xFF, sizeof img);
    float const height = (float)atof(argv[1]);
    float       y      = height * 1.4f;
    for (int i = 2; i < argc; i++) {
        float       x = 8.0f;
        char const* s = argv[i];
        for (;;) {
            uint32_t  cp;
            int const used = hershey_utf8_next(s, &cp);
            if (used == 0) break;
            s += used;
            if (cp != 0) x += draw_cp(x, y, cp, height);
        }
        y += height * 1.9f;
    }
    int const rows = (int)(y + height) < H ? (int)(y + height) : H;
    printf("P5\n%d %d\n255\n", W, rows);
    fwrite(img, 1, (size_t)rows * W, stdout);
    return 0;
}
