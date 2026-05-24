#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  vector text
// ---------------------------------------------------------------------
//  Hershey vector-font text rendering. The glyph machinery and the
//  inline stroke helpers are engine-internal (src/internal/hershey*.h);
//  this header is the stable surface games draw text through. Part of
//  the semver'd public API (see se_version.h).
// =====================================================================

#include "pax_gfx.h"
#include "pax_text.h"

// Uncomment to use Hershey vector font instead of PAX bitmap font
#define USE_HERSHEY_FONT

// Uncomment to use direct pixel operations instead of PAX drawing primitives
// (only effective when USE_HERSHEY_FONT is enabled)
#define USE_HERSHEY_DIRECT

pax_vec2f rendertext_draw(pax_buf_t *buf, pax_col_t color, pax_font_t const *font, float font_size, float x, float y, char const *text);
pax_vec2f rendertext_size(pax_font_t const *font, float font_size, char const *text);

// The Hershey simplex glyph table, exposed for callers that draw their
// own vector strokes (e.g. text mapped onto 3D geometry) rather than
// using the helpers above. Defined once inside the engine. Format per
// glyph index (ASCII - 32, range 0..94):
//     glyph[0]            = vertex-pair count
//     glyph[1]            = horizontal advance (font units)
//     glyph[2 + 2*i + 0]  = vertex x        (font units)
//     glyph[2 + 2*i + 1]  = vertex y        (font units, Y up; 0=baseline,
//                                            21=cap height)
//     (-1, -1)            = pen-up (lift between strokes)
// PUBLIC, but the values themselves are font data, not API -- treat the
// table as read-only.
extern int simplex[95][112];
