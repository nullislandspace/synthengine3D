# Getting started

This walks from an empty app to one with a 3D scene, sound, a menu and a save
file. It assumes you're building a Tanmatsu graceloader app and have a working
toolchain (the [integration guide](integration.md) covers wiring the engine
into your build).

## 1. The smallest app

```c
#include "synthengine3d.h"

static void on_update(float dt, void* user) { (void)dt; (void)user; }

static void on_render(pax_buf_t* fb, void* user) {
    (void)user;
    render_set_camera(0.0f, 1.0f);
    scene_begin(fb);
    scene_tri(-1, 0, 4,  1, 0, 4,  0, 2, 4, 0xFFFF31F1u);
    scene_render(SE_RENDER_ZBUFFER);
}

void app_main(void) {
    static se_app_config_t  const cfg = { .f1_exits = true,
                                          .backdrop_argb = 0xFF101018u };
    static se_app_callbacks_t const cb = { .on_update = on_update,
                                           .on_render = on_render };
    se_run(&cfg, &cb, NULL);
}
```

`se_run` boots the device, then loops: clear to `backdrop_argb`, call
`on_update`, call `on_render`, blit at vsync. F1 returns to the launcher. See
[`../examples/minimal/`](../examples/minimal/) for the commented version.

## 2. Drawing

The renderer is world-space: **x = lateral, y = up, z = forward** (into the
screen), camera at z = 0. Set the camera once per frame, then submit triangles
and wireframe edges between `scene_begin()` and `scene_render()`:

```c
scene_begin(fb);
scene_tri(x0,y0,z0, x1,y1,z1, x2,y2,z2, 0xFFRRGGBBu);   // filled, z-tested
scene_line(x0,y0,z0, x1,y1,z1,           0xFFRRGGBBu);   // wireframe edge
scene_render(SE_RENDER_ZBUFFER);                          // rasterize the frame
```

Order doesn't matter — the z-buffer resolves visibility. For 2D overlays (HUD,
text) draw straight onto `fb` with `se_text.h` / `se_direct565.h` / PAX after
the scene. Full details: [renderer.md](renderer.md), [objects.md](objects.md).

## 3. Sound

The mixer is up already (the engine called `audio_mixer_init()` at boot). To
play music, implement a `music_source_t` (or use the built-in procedural one)
and hand it over; the mixer takes ownership:

```c
music_source_t* m = music_procedural_create(NULL, seed);  // NULL = synthwave preset
audio_mixer_set_music(m);          // NULL to stop; its shutdown() frees it
```

(Pass your own `se_music_config_t` instead of `NULL` for different music — see
[audio.md](audio.md#the-config-is-the-content-the-synth-is-the-engine).)

For short effects, embed an `sfx_voice_t` in your effect's state and register
it with `audio_mixer_register_voice()`. The host pushes mute toggles via
`audio_mixer_set_music_enabled()` / `audio_mixer_set_group_enabled()`. See
[audio.md](audio.md).

## 4. A menu

Describe a menu as data; the engine owns the cursor + drawing:

```c
static se_menu_row_t rows[] = {
    { .label = "Play",     .kind = SE_MENU_VAL_NONE },
    { .label = "Sound",    .kind = SE_MENU_VAL_CHECK, .checked = true },
    { .label = "Bright",   .kind = SE_MENU_VAL_RANGE, .range_pct = 70 },
};
static se_menu_def_t const def = { .title = "MENU", .rows = rows,
                                   .row_count = 3, .hint = "up/down, enter" };
static se_menu_t menu = { .def = &def };

// per frame, from on_render:
se_menu_draw(&menu, fb);
if (up)    se_menu_input(&menu, SE_MENU_ACT_UP);
if (down)  se_menu_input(&menu, SE_MENU_ACT_DOWN);
if (enter) { if (se_menu_input(&menu, SE_MENU_ACT_ACTIVATE) == SE_MENU_RESULT_ACTIVATED)
                 act_on(menu.cursor); }
```

You derive the `SE_MENU_ACT_*` from your own input (latched in `on_input`).
For remappable controls + a "press a key" rebind dialog, see
[ui.md](ui.md).

## 5. Saving

Declare a slot config with two callbacks that (de)serialise *your* struct;
the engine handles the files, the directory and a peek header for slot-select:

```c
static void ser(NbtWriter* w, void const* gd) { /* nbt_write_* your fields */ }
static void deser(NbtReader* r, void* gd)      { /* read known tags, skip rest */ }

se_save_config_t const cfg = { .dir = "/int/mygame", .game_name = "My Game",
    .game_version = 1, .serialize = ser, .deserialize = deser };
se_save_init(&cfg);                       // once, at boot

se_save_write_slot(0, SE_SAVE_MANUAL, &state, "stage 3 · 12000 pts");
se_save_load_slot(0, &state);             // default `state` first
```

See [save.md](save.md).

## 6. Configure

Anything compile-time — display size, projection focal length, audio gains,
the UI palette, the save-slot count — is an `#ifndef`-guarded macro in
`se_config.h`. Override by defining it before the engine sees it (a forced
include, or `-D`). See [configuration.md](configuration.md).

## Where to go next

- [architecture.md](architecture.md) — the run loop + the whole-engine picture.
- The per-subsystem docs linked above.
- [integration.md](integration.md) — both build modes + display porting.
