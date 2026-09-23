// =====================================================================
//  SynthEngine3D  --  UTF-8 text, and which glyph a codepoint draws as
// ---------------------------------------------------------------------
//  Between the two renderers (hershey_font.h through PAX, and
//  hershey_font_direct.h straight into the framebuffer) and the glyph
//  tables (hershey.h for ASCII, hershey_ext.h for everything else)
//  sits one question, asked once per character: given a codepoint,
//  what strokes, how wide, and is there an accent on top? This header
//  answers it, so both renderers agree to the pixel and `rendertext_size`
//  can never disagree with `rendertext_draw`.
//
//  A string is UTF-8. A byte the decoder cannot make sense of is one
//  character wide and draws nothing -- text with a torn byte in it comes
//  out short, not scrambled, and never reads past the NUL.
//
//  Engine-internal (src/internal/): may change in any release.
// =====================================================================

#ifndef HERSHEY_TEXT_H
#define HERSHEY_TEXT_H

#include <stdbool.h>
#include <stdint.h>

#include "hershey.h"
#include "hershey_ext.h"

// The cap line and the x-height, in font units. Hershey's capitals reach
// 21 and his lowercase 14; the accents sit just clear of each, except
// those the table marks as hanging below the baseline.
#define HERSHEY_CAP_H  21
#define HERSHEY_X_H    14

// A codepoint with no glyph draws an empty box, the width of one. Better
// a row of boxes in a translation nobody checked than text that silently
// loses its letters.
#define HERSHEY_TOFU_ADV 12

// What one character draws. Exactly one of `pts32` (a simplex row, ints)
// and `pts8` (an ext glyph, int8 pairs) is set, unless `n` is 0.
typedef struct {
    int           adv;       // advance, font units
    int const*    pts32;     // first point of a simplex row, or NULL
    int8_t const* pts8;      // first point of an ext glyph, or NULL
    int           n;         // how many (x, y) pairs
    uint8_t       accent;    // SE_ACCENT_*, SE_ACCENT_NONE for most
    int8_t        accent_x;  // where the accent goes, font units, from the
    int8_t        accent_y;  // glyph's own origin
} hershey_glyph_t;

// --- UTF-8 ------------------------------------------------------------------

// Decode one character. Returns how many bytes it took (never 0 unless the
// string has ended); *cp is the codepoint, or 0 for a byte that is not
// valid UTF-8.
static inline int hershey_utf8_next(char const* s, uint32_t* cp) {
    unsigned char const c0 = (unsigned char)s[0];
    if (c0 == 0) {
        *cp = 0;
        return 0;
    }
    if (c0 < 0x80) {
        *cp = c0;
        return 1;
    }
    int      want;
    uint32_t v;
    if ((c0 & 0xE0) == 0xC0) {
        want = 1;
        v = c0 & 0x1Fu;
    } else if ((c0 & 0xF0) == 0xE0) {
        want = 2;
        v = c0 & 0x0Fu;
    } else if ((c0 & 0xF8) == 0xF0) {
        want = 3;
        v = c0 & 0x07u;
    } else {
        *cp = 0;  // a continuation byte, or 0xFE/0xFF: not a character at all
        return 1;
    }
    for (int i = 1; i <= want; i++) {
        unsigned char const c = (unsigned char)s[i];
        if ((c & 0xC0) != 0x80) {
            *cp = 0;  // truncated: stop where the good bytes stopped
            return i;
        }
        v = (v << 6) | (c & 0x3Fu);
    }
    *cp = v;
    return want + 1;
}

// --- Which glyph ------------------------------------------------------------

static inline bool hershey_ascii_glyph(uint32_t cp, hershey_glyph_t* g) {
    if (cp < 32 || cp > 126) return false;
    int const* row = simplex[cp - 32];
    g->adv         = row[1];
    g->pts32       = row + 2;
    g->pts8        = NULL;
    g->n           = row[0];
    g->accent      = SE_ACCENT_NONE;
    return true;
}

static inline bool hershey_ext_glyph(uint32_t cp, hershey_glyph_t* g) {
    int lo = 0, hi = SE_HERSHEY_EXT_COUNT - 1;
    while (lo <= hi) {
        int const mid = (lo + hi) / 2;
        uint32_t const at = SE_HERSHEY_EXT[mid].cp;
        if (at == cp) {
            g->adv    = SE_HERSHEY_EXT[mid].adv;
            g->pts32  = NULL;
            g->pts8   = &SE_HERSHEY_EXT_PTS[2 * SE_HERSHEY_EXT[mid].off];
            g->n      = SE_HERSHEY_EXT[mid].n;
            g->accent = SE_ACCENT_NONE;
            return true;
        }
        if (at < cp) lo = mid + 1;
        else         hi = mid - 1;
    }
    return false;
}

// Everything: ASCII, the folded characters, the ext glyphs, and the
// composed ones. False for a codepoint this font cannot draw -- the
// renderers then draw an empty box in its place, and the game's own
// checks are what stop such a character reaching a player (CraftMiner's
// `langcheck`, for one).
static inline bool hershey_glyph(uint32_t cp, hershey_glyph_t* g) {
    if (hershey_ascii_glyph(cp, g)) return true;

    for (int i = 0; i < SE_HERSHEY_FOLD_COUNT; i++) {
        if (SE_HERSHEY_FOLD[i].cp == cp) {
            uint8_t const to = SE_HERSHEY_FOLD[i].to;
            if (to == 0) {
                g->adv = 0;
                g->n = 0;
                g->pts32 = NULL;
                g->pts8 = NULL;
                g->accent = SE_ACCENT_NONE;
                return true;
            }
            return hershey_ascii_glyph(to, g);
        }
    }

    if (hershey_ext_glyph(cp, g)) return true;

    for (int i = 0; i < SE_HERSHEY_COMPOSED_COUNT; i++) {
        if (SE_HERSHEY_COMPOSED[i].cp != cp) continue;
        uint32_t const base = SE_HERSHEY_COMPOSED[i].base;
        if (!hershey_ascii_glyph(base, g) && !hershey_ext_glyph(base, g)) return false;
        g->accent = SE_HERSHEY_COMPOSED[i].accent;
        // Over the middle of the letter, and clear of whatever is tallest
        // in it: the cap line for a capital, the x-height for the rest --
        // except `i` and `j`, whose dot is already up there.
        g->accent_x            = (int8_t)(g->adv / 2);
        bool const is_capital  = (base >= 'A' && base <= 'Z') ||       // Latin
                                 (base >= 0x0391 && base <= 0x03A9) ||  // Greek
                                 (base >= 0x0410 && base <= 0x042F);    // Cyrillic
        if (is_capital) {
            g->accent_y = HERSHEY_CAP_H + 2;
        } else if (base == 'i' || base == 'j') {
            g->accent_y = HERSHEY_CAP_H + 1;
        } else {
            g->accent_y = HERSHEY_X_H + 2;
        }
        if (SE_HERSHEY_ACCENT[g->accent].below) {
            g->accent_y = 0;  // a cedilla, an ogonek, a comma: below instead
        }
        return true;
    }
    return false;
}

// The advance of a character, the width of the empty box if the font
// cannot draw it -- what the renderers put there.
static inline int hershey_advance(uint32_t cp) {
    hershey_glyph_t g;
    return hershey_glyph(cp, &g) ? g.adv : HERSHEY_TOFU_ADV;
}

#endif  // HERSHEY_TEXT_H
