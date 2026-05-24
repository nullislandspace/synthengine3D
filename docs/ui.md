# UI: menus + input bindings (`se_ui.h`, `se_bindings.h`)

Two cooperating pieces: a **data-driven list menu** renderer, and a
**remappable key-binding** store with a built-in rebind dialog.

## Menus (`se_ui.h`)

You describe a menu as data; the engine owns the cursor state machine and the
drawing. The game owns what each row *is* (its data) and what activating it
*does* — so one renderer serves every menu.

```c
typedef struct {
    char const*           label;
    se_menu_val_t         kind;       // NONE / CHECK / TEXT / CUSTOM / RANGE
    bool                  checked;    // CHECK:  [X] / [ ]
    char const*           value;      // TEXT:   free value string
    int                   range_pct;  // RANGE:  0..100 slider
    se_menu_draw_value_fn draw_value; // CUSTOM: game draws the value column
    void*                 ctx;        // CUSTOM: context for draw_value
} se_menu_row_t;

typedef struct {
    char const*          title, *subtitle, *hint;
    se_menu_row_t const* rows;  int row_count;
    float title_h, row_h, value_dx, panel_w, panel_h;  // 0 -> sensible default
} se_menu_def_t;

typedef struct { se_menu_def_t const* def; int cursor; } se_menu_t;
```

Per frame: draw it, then feed it the nav actions you detected this frame.

```c
se_menu_draw(&menu, fb);                 // dim panel + title + rows + hint

if (up)        se_menu_input(&menu, SE_MENU_ACT_UP);     // move + clamp, no wrap
if (down)      se_menu_input(&menu, SE_MENU_ACT_DOWN);
if (left)      r = se_menu_input(&menu, SE_MENU_ACT_LEFT);   // RANGE rows only
if (right)     r = se_menu_input(&menu, SE_MENU_ACT_RIGHT);
if (enter)     r = se_menu_input(&menu, SE_MENU_ACT_ACTIVATE);
if (escape)    r = se_menu_input(&menu, SE_MENU_ACT_BACK);
```

`se_menu_input` returns a `se_menu_result_t`:

| Result | Meaning |
|---|---|
| `SE_MENU_RESULT_NONE` | nothing actionable (cursor may have moved) |
| `SE_MENU_RESULT_ACTIVATED` | current row activated — read `menu.cursor` |
| `SE_MENU_RESULT_DECREMENT` / `_INCREMENT` | LEFT/RIGHT on a **RANGE** row — read `menu.cursor`, adjust your value |
| `SE_MENU_RESULT_BACK` | back / cancel |

The engine **owns no values** — it reports *which row + which direction*; the
game maps that onto whatever the row controls (mirrors how ACTIVATE works).
That keeps the menu reusable: a brightness slider, a volume slider and a
checkbox all go through the same renderer; the game decides what each does.

A **CUSTOM** row draws its own value column via `draw_value(fb, x, y, h, col,
ctx)` — e.g. rendering a bound key as a keycap icon while the engine draws the
rest of the row.

**Theme + geometry** (colours, paddings, slider bar size) are `SE_UI_*` macros
in [`se_config.h`](configuration.md); override them to reskin menus without
touching engine code.

You build the `SE_MENU_ACT_*` from your own input however you like (Race the
Synth maps its latched menu-nav / confirm / cancel onto them), so the menu has
no opinion about your input channels. Composes over live content too — draw a
pause menu on top of a frozen scene by drawing the scene first, then the menu.

## Input bindings (`se_bindings.h`)

Remappable, NVS-persisted controls. The game **declares** its control set; the
engine **owns** loading (with declared defaults as fallback), persisting, and
answering the current scancode.

```c
static se_binding_def_t const BINDINGS[] = {
    { .id = CTRL_LEFT,  .label = "Left",     .nvs_key = "k_left",  .default_sc = ESC_SCANCODE },
    { .id = CTRL_RIGHT, .label = "Right",    .nvs_key = "k_right", .default_sc = BKSP_SCANCODE },
    { .id = CTRL_ITEM,  .label = "Use item", .nvs_key = "k_item",  .default_sc = SPACE_SCANCODE },
};
static se_bindings_config_t const CFG = { .nvs_namespace = "mygame",
    .defs = BINDINGS, .count = 3 };

se_bindings_init(&CFG);                        // once at boot
uint16_t sc = se_bindings_get(CTRL_LEFT);      // cheap; poll for steering
se_bindings_set(CTRL_LEFT, new_scancode);      // rebind + persist
```

Bindings are raw **BSP scancodes**: every physical key has one, so a scancode
both polls cleanly for smooth steering (`bsp_input_read_scancode`) and matches
key events for edges. `defs` is held by reference — point it at static storage.
`count` is clamped to `SE_BINDINGS_MAX` (see `se_config.h`).

### The rebind dialog

`se_ui_capture_key()` (declared in `se_ui.h`, implemented in the run loop
because it needs the frame primitives) is a **blocking "press a key" modal**:
it pumps engine frames — drawing your registered `on_backdrop` plus a prompt
panel — until the user presses a bindable key, then returns its scancode. It
drains the input queue itself, so even keys the run loop would normally consume
(volume, F1) can be bound.

```c
uint16_t sc = se_ui_capture_key("Left");       // shows "Press a key — Left"
if (sc != 0) se_bindings_set(CTRL_LEFT, sc);    // 0 only if the loop is exiting
```

A typical Controls menu pairs the two: a CUSTOM row per control draws the bound
key (via the game's keycap drawer), and activating it calls `se_ui_capture_key`
→ `se_bindings_set`. The engine thus owns the storage, the persistence and the
rebind UI; the game just declares its controls and queries the mapping.
