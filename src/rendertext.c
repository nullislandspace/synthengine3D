#include "se_text.h"

#if defined(USE_HERSHEY_FONT) && defined(USE_HERSHEY_DIRECT)
#include "hershey_font_direct.h"
#elif defined(USE_HERSHEY_FONT)
#include "hershey_font.h"
#endif

#ifdef USE_HERSHEY_FONT
// Hershey capital height is 21 units but total glyph height (with descenders)
// is ~28 units. Scale down so total height matches the requested font_size.
#define HERSHEY_SCALE (21.0f / 28.0f)
#endif

pax_vec2f rendertext_draw(pax_buf_t *buf, pax_col_t color, pax_font_t const *font, float font_size, float x, float y, char const *text) {
#if defined(USE_HERSHEY_FONT) && defined(USE_HERSHEY_DIRECT)
    (void)font;
    return hershey_direct_draw_string(buf, color, x, y, text, font_size * HERSHEY_SCALE);
#elif defined(USE_HERSHEY_FONT)
    (void)font;
    return hershey_draw_string(buf, color, x, y, text, font_size * HERSHEY_SCALE);
#else
    return pax_draw_text(buf, color, font, font_size, x, y, text);
#endif
}

pax_vec2f rendertext_size(pax_font_t const *font, float font_size, char const *text) {
#if defined(USE_HERSHEY_FONT) && defined(USE_HERSHEY_DIRECT)
    (void)font;
    return hershey_direct_string_size(font_size * HERSHEY_SCALE, text);
#elif defined(USE_HERSHEY_FONT)
    (void)font;
    return hershey_string_size(font_size * HERSHEY_SCALE, text);
#else
    return pax_text_size(font, font_size, text);
#endif
}
