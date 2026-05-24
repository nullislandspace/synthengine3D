# Configuration (`se_config.h`)

Every value the engine needs to know at compile time — display geometry,
projection, audio gains, the UI theme, buffer caps — is a single
**`#ifndef`-guarded macro** in `se_config.h` with a sensible default. Values
that don't need compile-time folding are configured at runtime through the
relevant subsystem's init struct instead (e.g. save dir, binding set, app
callbacks).

## How to override

Define the macro **before `se_config.h` is reached**, two ways:

- **A forced include / game header pulled in first.** Race the Synth keeps its
  overrides in `magicnumbers.h`, which `#include`s `se_config.h` after its own
  `#define`s so game code sees the same values.
- **`-D` on the compile command** — e.g. `target_compile_definitions(... -DRENDER_HORIZON_Y=300)`.

Because each macro is `#ifndef`-guarded, your define wins and the default is
skipped. The macros fold into hot loops (the projection, the RGB565 leaves), so
they must be compile-time — that's why this is a header, not a runtime struct.

## What's in there

Grouped by subsystem (see each macro's comment in the header for the full
rationale; this is the map):

### Display / framebuffer geometry
- `DISPLAY_RAW_W` / `DISPLAY_RAW_H` — the LCD's native (unrotated) pixel
  dimensions.
- `DISPLAY_LOG_W` / `DISPLAY_LOG_H` — logical dimensions after the orientation
  transform (under `PAX_O_ROT_CW`, logical W = raw H and vice-versa).
- `DISPLAY_RAW_STRIDE` — raw row stride in pixels.

These are *the* knobs to **port to a different display** — the direct-565
leaves hardcode rotation + stride into their inner loops off these.

### Renderer / projection (`se_scene`)
- `RENDER_HALF_W` — screen x of the optical centre (default `DISPLAY_LOG_W/2`).
- `RENDER_HORIZON_Y` — the screen row the horizon vanishes to; **set this to
  line the 3D scene up with your backdrop.**
- `RENDER_FOCAL_LEN` — focal length (lateral FOV).
- `RENDER_CAM_Y` — the camera's resting eye height.
- `RENDER_NEAR_CLIP_Z` — near plane; geometry fully behind it is dropped.

### Audio (`se_audio`)
- `AUDIO_MUSIC_GAIN` / `AUDIO_SFX_GAIN` — master music-vs-SFX balance (Q15 at
  mix-down).
- `SE_AUDIO_SFX_GROUP_COUNT` — number of independent SFX mute groups.

### Run loop (`se_run`)
- `SE_FRAME_DT_MAX` — the per-frame delta-time is clamped to at most this
  before `on_update`, so a long stall can't teleport the simulation.

### Device settings (`se_hw`)
- `SE_HW_VOLUME_STEP_PCT` — % step for the volume keys / slider.
- `SE_HW_BRIGHTNESS_STEP_PCT` — % step for the brightness sliders.
- `SE_HW_DISPLAY_BRIGHTNESS_MIN` — floor for the *display* backlight, so a
  slider sweep can't black the screen out and trap the user (keyboard/LED have
  no floor).

### Bindings (`se_bindings`)
- `SE_BINDINGS_MAX` — max remappable controls a game may declare.

### PPA compositor (`se_ppa`)
- `SE_PPA_MAX_PENDING` — in-flight op cap. Sizes *both* the completion semaphore
  and the submit guard from one value (so they can't drift); a submit past it is
  refused (returns `false`), never silently dropped. Raise for an app batching
  many async blits.
- `SE_PPA_CLIENT_QUEUE_DEPTH` — per-client queue depth (PPA `max_pending_trans_num`).
- `SE_PPA_CACHE_LINE` — PSRAM cache-line size (bytes) for layer-cache aligned
  allocation + the `esp_cache_msync` flush; 128 on the ESP32-P4.

### Save (`se_save`)
- `SE_SAVE_SLOT_COUNT` — number of save slots.

### UI theme + geometry (`se_ui`)
- `SE_UI_COL_TITLE` / `_HILITE` / `_NORMAL` / `_HINT` / `_SUB` — menu palette
  (ARGB8888).
- `SE_UI_TEXT_INSET` / `_CHEVRON_GUTTER` / `_TOP_PAD` / `_FOOTER_PAD` /
  `_ROW_TEXT_H` — menu panel layout.
- `SE_UI_BAR_W` / `_BAR_H` — the RANGE-row slider bar size.

Override the `SE_UI_*` set to reskin menus without touching engine code.

## Reading values at runtime

`se_config.h` is just macros; include it (or the umbrella) and use them. For the
display geometry the engine actually resolved from the BSP at boot — which is
what you need for auxiliary buffers — call `se_display_info()` (see
[architecture.md](architecture.md)) rather than assuming the `DISPLAY_*`
defaults; the macros are the *compile-time* contract, `se_display_info()` is the
*runtime* truth.
