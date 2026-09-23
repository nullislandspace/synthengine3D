// =====================================================================
//  SynthEngine3D  --  UI / list-menu system (see se_ui.h)
// ---------------------------------------------------------------------
//  Lifted from the game's data-driven list-menu renderer so every
//  Tanmatsu app shares one. Renders through the engine's own inline
//  leaves only (se_text vector text + se_direct565 panel dim); the
//  cursor state machine lives here too. A row that needs game-specific
//  value drawing uses SE_MENU_VAL_CUSTOM + a draw_value callback.
// =====================================================================

#include "se_ui.h"

#include <stdio.h>          // snprintf (RANGE percentage label)

#include "se_config.h"      // SE_UI_COL_*, SE_UI_* geometry
#include "se_direct565.h"   // direct_565_dim_rect / line / vrun
#include "se_text.h"        // rendertext_draw

// Layout fallbacks for se_menu_def_t fields left at 0.
#define SE_UI_DEF_TITLE_H  36.0f
#define SE_UI_DEF_ROW_H    44.0f
#define SE_UI_DEF_PANEL_W  0.60f
#define SE_UI_DEF_PANEL_H  0.70f

static float pick(float v, float fallback) {
    return (v > 0.0f) ? v : fallback;
}

// Left-aligned vector text -- the one primitive every menu element uses.
static void draw_left(pax_buf_t* fb, float x, float y, float h,
                      pax_col_t color, char const* text) {
    rendertext_draw(fb, color, NULL, h, x, y, text);
}

// Solid logical-rect fill via the direct-565 column primitive (one
// contiguous raw run per column under PAX_O_ROT_CW). Used only by the
// RANGE slider -- a once-per-frame menu element, not a hot path.
static void fill_rect_565(uint16_t* px, int x, int y, int w, int h, uint16_t packed) {
    for (int i = 0; i < w; i++) {
        direct_565_vrun(px, x + i, y, y + h - 1, packed);
    }
}

// Single-pixel rectangle outline (four direct-565 lines).
static void outline_rect_565(uint16_t* px, int x, int y, int w, int h, uint16_t packed) {
    direct_565_line(px, x,         y,         x + w - 1, y,         packed);
    direct_565_line(px, x,         y + h - 1, x + w - 1, y + h - 1, packed);
    direct_565_line(px, x,         y,         x,         y + h - 1, packed);
    direct_565_line(px, x + w - 1, y,         x + w - 1, y + h - 1, packed);
}

// A circle outline, and a filled one, by the midpoint algorithm at a
// size where a rasterised circle still reads as round (about 14 px).
static void circle_565(uint16_t* px, int cx, int cy, int r, uint16_t packed, bool fill) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        if (fill) {
            direct_565_line(px, cx - x, cy + y, cx + x, cy + y, packed);
            direct_565_line(px, cx - x, cy - y, cx + x, cy - y, packed);
            direct_565_line(px, cx - y, cy + x, cx + y, cy + x, packed);
            direct_565_line(px, cx - y, cy - x, cx + y, cy - x, packed);
        } else {
            direct_565_set_pixel(px, cx + x, cy + y, packed);
            direct_565_set_pixel(px, cx - x, cy + y, packed);
            direct_565_set_pixel(px, cx + x, cy - y, packed);
            direct_565_set_pixel(px, cx - x, cy - y, packed);
            direct_565_set_pixel(px, cx + y, cy + x, packed);
            direct_565_set_pixel(px, cx - y, cy + x, packed);
            direct_565_set_pixel(px, cx + y, cy - x, packed);
            direct_565_set_pixel(px, cx - y, cy - x, packed);
        }
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

// Draw a SE_MENU_VAL_RADIO dot: a ring, filled in the middle when this
// is the chosen row. `x` is the value column's left edge, `ry` the row's
// text top, `th` the row text height. Centred on the text caps, so it
// sits on the same line as a "[X]" would.
static void draw_radio(pax_buf_t* fb, float x, float ry, float th, pax_col_t col, bool on) {
    uint16_t* const out    = (uint16_t*)pax_buf_get_pixels(fb);
    uint16_t  const packed = direct_565_pack_for(fb, col);
    int const       r      = (int)(th * SE_UI_RADIO_R);
    int const       cx     = (int)x + r + 1;
    int const       cy     = (int)(ry + th * 0.5f);
    circle_565(out, cx, cy, r, packed, false);
    if (on) circle_565(out, cx, cy, r - 3 > 1 ? r - 3 : 1, packed, true);
}

// Draw a SE_MENU_VAL_RANGE slider: an outlined track filled to `pct`%,
// then the "NN%" readout to its right. `x` is the value column's left
// edge, `ry` the row's text top, `th` the row text height, `col` the
// row's current colour. The bar is centred vertically on the text caps.
static void draw_range(pax_buf_t* fb, float x, float ry, float th,
                       pax_col_t col, int pct) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    uint16_t* const out    = (uint16_t*)pax_buf_get_pixels(fb);
    uint16_t  const packed = direct_565_pack_for(fb, col);

    int const bx = (int)x;
    int const bw = (int)SE_UI_BAR_W;
    int const bh = (int)SE_UI_BAR_H;
    int const by = (int)(ry + (th - SE_UI_BAR_H) * 0.5f);

    outline_rect_565(out, bx, by, bw, bh, packed);
    // Inner fill, inset 2 px from the outline so the track edge stays
    // visible at 100%; width tracks the percentage.
    int const inner_w = bw - 4;
    int const fill_w  = (inner_w * pct) / 100;
    if (fill_w > 0) {
        fill_rect_565(out, bx + 2, by + 2, fill_w, bh - 4, packed);
    }

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    draw_left(fb, x + SE_UI_BAR_W + 14.0f, ry, th, col, buf);
}

se_menu_result_t se_menu_input(se_menu_t* menu, se_menu_action_t action) {
    if (menu == NULL || menu->def == NULL) return SE_MENU_RESULT_NONE;
    int const n = menu->def->row_count;
    switch (action) {
        case SE_MENU_ACT_UP:
            if (n > 0) {
                menu->cursor--;
                if (menu->cursor < 0) menu->cursor = 0;
            }
            return SE_MENU_RESULT_NONE;
        case SE_MENU_ACT_DOWN:
            if (n > 0) {
                menu->cursor++;
                if (menu->cursor >= n) menu->cursor = n - 1;
            }
            return SE_MENU_RESULT_NONE;
        case SE_MENU_ACT_LEFT:
        case SE_MENU_ACT_RIGHT:
            // Only meaningful on a RANGE row; the game adjusts whatever
            // value menu->cursor controls. Any other row ignores it.
            if (n > 0 && menu->cursor >= 0 && menu->cursor < n
                && menu->def->rows[menu->cursor].kind == SE_MENU_VAL_RANGE) {
                return (action == SE_MENU_ACT_LEFT) ? SE_MENU_RESULT_DECREMENT
                                                    : SE_MENU_RESULT_INCREMENT;
            }
            return SE_MENU_RESULT_NONE;
        case SE_MENU_ACT_ACTIVATE:
            return SE_MENU_RESULT_ACTIVATED;
        case SE_MENU_ACT_BACK:
            return SE_MENU_RESULT_BACK;
        case SE_MENU_ACT_NONE:
        default:
            return SE_MENU_RESULT_NONE;
    }
}

void se_menu_draw(se_menu_t const* menu, pax_buf_t* fb) {
    if (menu == NULL || menu->def == NULL || fb == NULL) return;
    se_menu_def_t const* m = menu->def;

    float const fbw     = pax_buf_get_widthf(fb);
    float const fbh     = pax_buf_get_heightf(fb);
    float const panel_wf = pick(m->panel_w, SE_UI_DEF_PANEL_W);
    float const panel_hf = pick(m->panel_h, SE_UI_DEF_PANEL_H);
    float const title_h  = pick(m->title_h, SE_UI_DEF_TITLE_H);
    float const row_h    = pick(m->row_h,   SE_UI_DEF_ROW_H);

    // Dim panel, centred.
    int const pw = (int)(fbw * panel_wf);
    int const ph = (int)(fbh * panel_hf);
    int const px = (int)((fbw - (float)pw) * 0.5f);
    int const py = (int)((fbh - (float)ph) * 0.5f);
    direct_565_dim_rect((uint16_t*)pax_buf_get_pixels(fb), fb->reverse_endianness,
                        px, py, pw, ph);

    float const panel_x = (fbw - fbw * panel_wf) * 0.5f;
    float const panel_y = (fbh - fbh * panel_hf) * 0.5f;
    float const panel_h = fbh * panel_hf;

    float const chevron_x = panel_x + SE_UI_TEXT_INSET;
    float const text_x    = chevron_x + SE_UI_CHEVRON_GUTTER;
    float const value_x   = text_x + m->value_dx;

    float y = panel_y + SE_UI_TOP_PAD;
    if (m->title) {
        draw_left(fb, text_x, y, title_h, SE_UI_COL_TITLE, m->title);
    }
    y += title_h + 14.0f;
    if (m->subtitle) {
        draw_left(fb, text_x, y, 18.0f, SE_UI_COL_NORMAL, m->subtitle);
        y += 18.0f + 16.0f;
    } else {
        y += 14.0f;
    }

    // The window of rows on show. Stateless: it is worked out from the
    // cursor every frame, centred on it where the list allows, so there
    // is no scroll position for a game to keep or get out of step with.
    int first = 0, shown = m->row_count;
    if (m->visible_rows > 0 && m->row_count > m->visible_rows) {
        shown = m->visible_rows;
        first = menu->cursor - shown / 2;
        if (first > m->row_count - shown) first = m->row_count - shown;
        if (first < 0) first = 0;
        // More above / more below, in the chevron gutter: a small caret
        // just outside the first and last rows shown.
        if (first > 0) {
            draw_left(fb, chevron_x, y - row_h * 0.45f, 16.0f, SE_UI_COL_HINT, "^");
        }
        if (first + shown < m->row_count) {
            draw_left(fb, chevron_x, y + (float)shown * row_h - row_h * 0.30f, 16.0f, SE_UI_COL_HINT, "v");
        }
    }

    for (int k = 0; k < shown; k++) {
        int const            i   = first + k;
        se_menu_row_t const* r   = &m->rows[i];
        bool const           sel = (i == menu->cursor);
        pax_col_t  const     col = sel ? SE_UI_COL_HILITE : SE_UI_COL_NORMAL;
        float const          ry  = y + (float)k * row_h;
        if (sel) {
            draw_left(fb, chevron_x, ry, SE_UI_ROW_TEXT_H, SE_UI_COL_HILITE, ">");
        }
        if (r->label) {
            draw_left(fb, text_x, ry, SE_UI_ROW_TEXT_H, col, r->label);
        }
        switch (r->kind) {
            case SE_MENU_VAL_CHECK:
                draw_left(fb, value_x, ry, SE_UI_ROW_TEXT_H, col,
                          r->checked ? "[X]" : "[ ]");
                break;
            case SE_MENU_VAL_TEXT:
                if (r->value) {
                    draw_left(fb, value_x, ry, SE_UI_ROW_TEXT_H, col, r->value);
                }
                break;
            case SE_MENU_VAL_CUSTOM:
                if (r->draw_value) {
                    r->draw_value(fb, value_x, ry, SE_UI_ROW_TEXT_H, col, r->ctx);
                }
                break;
            case SE_MENU_VAL_RANGE:
                draw_range(fb, value_x, ry, SE_UI_ROW_TEXT_H, col, r->range_pct);
                break;
            case SE_MENU_VAL_RADIO:
                draw_radio(fb, value_x, ry, SE_UI_ROW_TEXT_H, col, r->checked);
                break;
            case SE_MENU_VAL_NONE:
            default:
                break;
        }
    }

    if (m->hint) {
        draw_left(fb, text_x, panel_y + panel_h - SE_UI_FOOTER_PAD, 14.0f,
                  SE_UI_COL_HINT, m->hint);
    }
}
