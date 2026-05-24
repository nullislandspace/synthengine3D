#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  (hot inline framebuffer leaf)
// ---------------------------------------------------------------------
//  Direct RGB565 pixel / line / triangle primitives. These are
//  performance-critical `static inline` leaves: they MUST stay in this
//  public header so callers (engine and game) inline them -- never move
//  them behind a function-call or opaque-handle boundary. Display
//  geometry comes from se_config.h (overridable). Part of the stable,
//  semver'd surface (see se_version.h).
// =====================================================================

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "se_config.h"
#include "pax_gfx.h"

// Direct RGB565 pixel / line primitives.
//
// Bypasses PAX's per-pixel function-pointer setter dispatch. The
// inner loops know at compile time that:
//   * the framebuffer is RGB565 (16 bpp halfword stores)
//   * the orientation is PAX_O_ROT_CW (raw_x = RAW_W - 1 - log_y,
//     raw_y = log_x — see `direct_565_logical_index` below)
//   * the raw stride is `DISPLAY_RAW_STRIDE` pixels
// so no orientation switch, no buffer-field reads, and no
// multiplies per pixel. Stepping logical y by ±1 is a pointer
// step of ∓1; stepping logical x by ±1 is a pointer step of
// ±DISPLAY_RAW_STRIDE. Everything else is constant-folded by the
// compiler.
//
// Color conversion (ARGB8888 → 565) happens once per primitive via
// `direct_565_pack`/`direct_565_pack_for`, not per pixel. The
// caller passes the pre-packed halfword into the line/pixel
// functions.

// Pack an ARGB8888 pax_col_t into an RGB565 halfword in the
// framebuffer's native byte order. The buffer's endian-reverse
// flag matters because RGB565 may be stored byte-swapped depending
// on the LCD's expected wire format.
static inline uint16_t direct_565_pack(pax_col_t color, bool reverse_endian) {
    uint8_t const r = (color >> 16) & 0xFF;
    uint8_t const g = (color >>  8) & 0xFF;
    uint8_t const b =  color        & 0xFF;
    uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return reverse_endian ? (uint16_t)__builtin_bswap16(v) : v;
}

// Convenience wrapper: pack a color in the byte order matching the
// given pax_buf_t. The buffer's `reverse_endianness` field is set
// up at boot to match the LCD's expected RGB565 byte order.
static inline uint16_t direct_565_pack_for(pax_buf_t const* buf, pax_col_t color) {
    return direct_565_pack(color, buf->reverse_endianness);
}

// Map logical (lx, ly) to a flat pixel index in the raw framebuffer.
// PAX_O_ROT_CW: raw_x = DISPLAY_RAW_W - 1 - ly, raw_y = lx.
//   index = raw_y * stride + raw_x
//         = lx * DISPLAY_RAW_STRIDE + (DISPLAY_RAW_W - 1 - ly)
static inline int direct_565_logical_index(int lx, int ly) {
    return lx * DISPLAY_RAW_STRIDE + (DISPLAY_RAW_W - 1 - ly);
}

// Plot a single pixel. Bounds-checked.
static inline void direct_565_set_pixel(uint16_t* pixels, int lx, int ly, uint16_t packed) {
    if (lx < 0 || lx >= DISPLAY_LOG_W || ly < 0 || ly >= DISPLAY_LOG_H) return;
    pixels[direct_565_logical_index(lx, ly)] = packed;
}

// Bresenham line from (x0, y0) to (x1, y1) inclusive, all in
// logical pixel coordinates. The inner loop carries a single
// uint16_t* — no multiplies, no orientation switches, just one
// halfword store + a couple of branches per pixel.
//
// `packed` must already be in the framebuffer's native byte order
// (use `direct_565_pack_for` or `direct_565_pack` to produce it).
static inline void direct_565_line(uint16_t* pixels, int x0, int y0, int x1, int y1, uint16_t packed) {
    int const dx = abs(x1 - x0);
    int const dy = abs(y1 - y0);
    int const sx = (x0 < x1) ? 1 : -1;
    int const sy = (y0 < y1) ? 1 : -1;
    int       err = dx - dy;

    // Pointer steps under PAX_O_ROT_CW.
    int const ptr_dx = (sx > 0) ? DISPLAY_RAW_STRIDE : -DISPLAY_RAW_STRIDE;
    int const ptr_dy = (sy > 0) ? -1 : 1;

    uint16_t* ptr = pixels + direct_565_logical_index(x0, y0);
    int       lx  = x0;
    int       ly  = y0;

    while (1) {
        if (lx >= 0 && lx < DISPLAY_LOG_W && ly >= 0 && ly < DISPLAY_LOG_H) {
            *ptr = packed;
        }
        if (lx == x1 && ly == y1) break;
        int const e2 = 2 * err;
        if (e2 > -dy) { err -= dy; lx += sx; ptr += ptr_dx; }
        if (e2 <  dx) { err += dx; ly += sy; ptr += ptr_dy; }
    }
}

// Translucent panel: blend each pixel inside the logical rectangle
// 50/50 with a dark grey, dimming the background so overlaid text
// reads cleanly. Standard RGB565 50% blend trick:
//   avg = ((a & 0xF7DE) >> 1) + ((b & 0xF7DE) >> 1)
// The mask clears each channel's LSB before the right-shift so
// channels don't bleed into each other. The grey-half constant
// (0x0861 = (0x18C3 & 0xF7DE) >> 1; 0x18C3 ≈ 10% RGB grey) is
// pre-computed, so the inner loop is one read + AND + shift + add
// + write per pixel. Mask and constant are byte-swapped when the
// framebuffer is stored in reverse endian — `+`, `&`, and `>> 1`
// all commute with byteswap on uint16_t, so the math is correct
// without per-pixel swapping.
static inline void direct_565_dim_rect(uint16_t* pixels, bool reverse_endian,
                                       int lx, int ly, int lw, int lh) {
    uint16_t const mask      = reverse_endian ? (uint16_t)__builtin_bswap16((uint16_t)0xF7DEu)
                                              : (uint16_t)0xF7DEu;
    // Pure 50% halve — no grey floor added. The math `(px & mask) >> 1`
    // produces a darkened pixel preserving the original hue at half
    // intensity, with no color shift. Any non-zero `grey_half` would
    // need its R, G, and B channel bits all set in the RGB565 layout
    // — bit 11 for R LSB, bit 5 for G LSB, bit 0 for B LSB; otherwise
    // the addition lands in only one channel and tints the panel
    // toward that colour. The simplest correct alternative is the
    // balanced LSB constant `0x0821` (adds R=1, G=1, B=1).

    if (lx < 0) { lw += lx; lx = 0; }
    if (ly < 0) { lh += ly; ly = 0; }
    if (lx + lw > DISPLAY_LOG_W) lw = DISPLAY_LOG_W - lx;
    if (ly + lh > DISPLAY_LOG_H) lh = DISPLAY_LOG_H - ly;
    if (lw <= 0 || lh <= 0) return;

    for (int x = lx; x < lx + lw; x++) {
        uint16_t* p = pixels + direct_565_logical_index(x, ly);
        // Pointer step for +1 logical y is -1 halfword under PAX_O_ROT_CW.
        for (int yi = 0; yi < lh; yi++) {
            uint16_t const px = *p;
            *p = (uint16_t)((px & mask) >> 1);
            p--;
        }
    }
}

// Internal: fill a vertical run of pixels (logical x fixed,
// logical y from y_top to y_bot inclusive). This is the inner
// loop of `direct_565_tri` — under PAX_O_ROT_CW it writes a
// *contiguous* raw byte range, so the cache line is reused for
// the whole run.
static inline void direct_565_vrun(uint16_t* pixels, int lx, int y_top, int y_bot, uint16_t packed) {
    if (lx < 0 || lx >= DISPLAY_LOG_W) return;
    if (y_top < 0)               y_top = 0;
    if (y_bot >= DISPLAY_LOG_H)   y_bot = DISPLAY_LOG_H - 1;
    if (y_top > y_bot) return;
    // direct_565_logical_index(lx, y) = lx * STRIDE + (RAW_W - 1 - y).
    // Stepping y by +1 → raw index decreases by 1 → ptr-- (halfword).
    uint16_t* p = pixels + direct_565_logical_index(lx, y_top);
    int count = y_bot - y_top + 1;
    while (count-- > 0) {
        *p-- = packed;
    }
}

// Flat-shaded filled triangle. Scans by *logical X* so each
// scanline maps to a contiguous raw memory run (cache-friendly
// under PAX_O_ROT_CW). Float endpoints; `packed` must already be
// in the framebuffer's native byte order.
//
// No sub-pixel correctness pass — vertices snap to integer
// columns via ceil/floor at edge boundaries. For solid-fill
// flat triangles sharing edges (cube faces) this can produce
// occasional 1-px gaps or overdraws at the shared edge; in
// practice the cyan wireframe overlay hides them and we don't
// see artefacts. Add half-space rasterisation later if it ever
// matters for a wireframe-free pipeline.
static inline void direct_565_tri(uint16_t* pixels,
                                  float x0, float y0,
                                  float x1, float y1,
                                  float x2, float y2,
                                  uint16_t packed) {
    // Sort vertices so x0 ≤ x1 ≤ x2.
    float tx, ty;
    if (x1 < x0) { tx=x0; ty=y0; x0=x1; y0=y1; x1=tx; y1=ty; }
    if (x2 < x0) { tx=x0; ty=y0; x0=x2; y0=y2; x2=tx; y2=ty; }
    if (x2 < x1) { tx=x1; ty=y1; x1=x2; y1=y2; x2=tx; y2=ty; }

    // Reject zero-area / fully off-screen triangles.
    if (x2 <= 0.0f || x0 >= (float)DISPLAY_LOG_W) return;
    if (x2 - x0 < 1e-6f) return;

    // Edge slopes dy/dx for each of the three edges.
    float const dydx_02 = (y2 - y0) / (x2 - x0);
    float const dydx_01 = (x1 > x0) ? (y1 - y0) / (x1 - x0) : 0.0f;
    float const dydx_12 = (x2 > x1) ? (y2 - y1) / (x2 - x1) : 0.0f;

    // Integer column ranges, clamped to the screen.
    int ix_start =   (int)ceilf(x0);
    int ix_split =   (int)ceilf(x1);
    int ix_endex = 1 + (int)floorf(x2);          // exclusive end
    if (ix_start < 0)                ix_start = 0;
    if (ix_endex > DISPLAY_LOG_W)    ix_endex = DISPLAY_LOG_W;
    if (ix_split < ix_start)         ix_split = ix_start;
    if (ix_split > ix_endex)         ix_split = ix_endex;

    // Left half: x ∈ [ix_start, ix_split). Edges are v0→v2 and v0→v1.
    for (int x = ix_start; x < ix_split; x++) {
        float const dx = (float)x - x0;
        float const ya = y0 + dydx_02 * dx;
        float const yb = y0 + dydx_01 * dx;
        float       yt, yz;
        if (ya < yb) { yt = ya; yz = yb; } else { yt = yb; yz = ya; }
        direct_565_vrun(pixels, x, (int)ceilf(yt), (int)floorf(yz), packed);
    }

    // Right half: x ∈ [ix_split, ix_endex). Edges are v0→v2 (continued)
    // and v1→v2.
    for (int x = ix_split; x < ix_endex; x++) {
        float const dx02 = (float)x - x0;
        float const dx12 = (float)x - x1;
        float const ya   = y0 + dydx_02 * dx02;
        float const yb   = y1 + dydx_12 * dx12;
        float       yt, yz;
        if (ya < yb) { yt = ya; yz = yb; } else { yt = yb; yz = ya; }
        direct_565_vrun(pixels, x, (int)ceilf(yt), (int)floorf(yz), packed);
    }
}
