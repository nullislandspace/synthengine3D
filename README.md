# SynthEngine3D

A small, reusable **game engine for [Tanmatsu](https://nicolaielectronics.nl/)
graceloader apps** (ESP32-P4). It owns the parts every such game re-writes —
the run loop, a software 3D renderer, an audio mixer, menus, input remapping,
device settings and save files — so a game is *content + per-frame logic*, not
boilerplate.

Extracted from the game **Race the Synth**; still vendored in that repo while
the API settles. Pre-1.0: the public surface is documented and semver'd, but
minor versions may still add (and, while < 1.0, occasionally change) it.

---

## What it gives you

| Subsystem | Header | What |
|---|---|---|
| **Application framework** | `se_run.h` | Inversion-of-control run loop: you call `se_run(&cfg, &cb, user)` once; the engine owns device bootstrap, the frame loop + delta-time, the input-queue pump, the device-global keys (volume / audio-jack / F1-exit), vsync + blit + double-buffer swap, and a backdrop hook. Your game is a set of callbacks. |
| **3D renderer** | `se_scene.h` | Per-pixel **z-buffered** software rasterizer + pinhole camera + projection. Submit world-space triangles / wireframe edges; the engine projects, depth-tests and draws. Deferred: it accumulates the frame then `scene_render()`s it, so it can own the algorithm (and, later, central cull + order). |
| **Audio** | `se_audio.h`, `se_audio_source.h`, `se_audio_dsp.h`, `se_music_procedural.h` | 22050 Hz / s16 / stereo software mixer over the BSP I2S channel: one music slot + N SFX voices, app-pushed mute groups, idle power-down. DSP primitives (oscillators, envelopes, biquad) and a config-driven, seed-derived procedural music source included (supply a `se_music_config_t` for your own music, or `NULL` for the synthwave preset). |
| **UI / menus** | `se_ui.h` | Data-driven vertical list menus (label / checkbox / value / slider / custom-drawn rows), an engine-owned cursor state machine, and a blocking "press a key" capture for rebinds. |
| **Input bindings** | `se_bindings.h` | Remappable, NVS-persisted key bindings: the game declares its controls + defaults; the engine loads, persists and answers them. |
| **Device settings** | `se_hw.h` | The launcher-shared hardware settings (speaker/headphone volume, screen/keyboard/LED brightness): applied at boot, adjustable in-game, persisted back so they carry across apps. |
| **Save framework** | `se_save.h`, `se_nbt.h` | N file-backed save slots with an engine-written "peek" header (timestamp / kind / a free-text summary) for slot-select screens. Game (de)serialises its own schema via two callbacks over the NBT primitive. |
| **Vector text** | `se_text.h` | Hershey single-stroke vector text, rendered straight to the framebuffer. |
| **Framebuffer leaves** | `se_direct565.h` | Hot `static inline` RGB565 pixel / line / triangle / dim-rect primitives (rotation + stride compile-folded). |
| **Configuration** | `se_config.h` | Every compile-time default (display geometry, projection, audio gains, UI theme, slot count, …) as an overridable `#ifndef` macro. |

Include everything via the umbrella `#include "synthengine3d.h"`, or pull
individual `se_*.h` headers.

---

## Quick start — hello triangle + sound

```c
#include "synthengine3d.h"

static float s_angle = 0.0f;

static void on_update(float dt, void* user) {
    (void)user;
    s_angle += dt;                 // spin
}

static void on_render(pax_buf_t* fb, void* user) {
    (void)user;
    render_set_camera(0.0f, 1.0f); // eye at x=0, height 1
    scene_begin(fb);
    float const c = cosf(s_angle), s = sinf(s_angle);
    // a triangle standing at z = 4, rotating about the vertical axis
    scene_tri(-c, 0.0f, 4.0f - s,   c, 0.0f, 4.0f + s,   0.0f, 2.0f, 4.0f,
              0xFFFF31F1u);
    scene_render(SE_RENDER_ZBUFFER);
}

void app_main(void) {
    static se_app_config_t const cfg = { .f1_exits = true,
                                         .backdrop_argb = 0xFF101018u };
    static se_app_callbacks_t const cb = { .on_update = on_update,
                                           .on_render = on_render };
    se_run(&cfg, &cb, NULL);        // never returns under graceloader
}
```

That is a complete graceloader app: the engine boots the device, clears the
screen to the backdrop colour each frame, runs your callbacks, blits at vsync,
and exits to the launcher on F1. See [`examples/minimal/`](examples/minimal/)
for the same thing with comments, and [`docs/getting-started.md`](docs/getting-started.md)
for adding audio, menus and a save file.

---

## Two build modes

The same `CMakeLists.txt` builds two ways (see [`docs/integration.md`](docs/integration.md)):

- **ESP-IDF component** — under `idf.py`, it registers a normal IDF component
  (`idf_component_register`), so a full IDF app (e.g. graceloader itself) can
  consume it, vendored or via the component registry (`idf_component.yml`).
- **Plain-CMake object library** — under a hand-rolled `app.so` build (how
  Race the Synth builds), it compiles to a relocatable OBJECT library folded
  into the app's shared object.

---

## Public vs internal, stability, performance

- **Public API = everything in `include/`** (the `se_*.h` headers + the
  `synthengine3d.h` umbrella). This is the semver'd surface.
- **Internal = everything in `src/`** (including `src/internal/`). Never
  include it from a game; it can change at any patch release.
- **Semver** (`se_version.h`): MAJOR = incompatible public change, MINOR =
  compatible additions, PATCH = internal-only. See [`CHANGELOG.md`](CHANGELOG.md).
- **Performance rule:** hot per-pixel leaves are `static inline` in public
  headers (`se_direct565.h`, `se_text.h`) and *must stay inline* — never move
  them behind a function-call/opaque boundary. Coarse, once-per-frame calls
  (mixer, save, menu draw) are ordinary functions. See
  [`docs/architecture.md`](docs/architecture.md#performance-contract).

---

## Docs

- [`docs/getting-started.md`](docs/getting-started.md) — build an app step by step.
- [`docs/architecture.md`](docs/architecture.md) — subsystems, the frame lifecycle, the IoC model, public vs internal, the performance contract.
- [`docs/renderer.md`](docs/renderer.md) — the deferred 3D pipeline, camera, projection, z-buffer.
- [`docs/audio.md`](docs/audio.md) — the mixer, source contracts, DSP, procedural music.
- [`docs/ui.md`](docs/ui.md) — menus + the input-bindings/remap flow.
- [`docs/save.md`](docs/save.md) — the save-slot framework + NBT.
- [`docs/objects.md`](docs/objects.md) — the geometry-submission contract for 3D objects.
- [`docs/configuration.md`](docs/configuration.md) — every `se_config.h` knob.
- [`docs/integration.md`](docs/integration.md) — both build modes, dependencies, porting the display.
