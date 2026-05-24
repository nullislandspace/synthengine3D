# Architecture

SynthEngine3D is a **framework**, not a toolkit: it runs the application and
calls into the game, rather than being a pile of helpers the game drives. This
inversion is the spine everything else hangs off.

## Inversion of control: the run loop

The game calls `se_run(&cfg, &cb, user)` exactly once (from `app_main`) and
never returns under graceloader. The engine owns:

- **device + subsystem bootstrap** — NVS, BSP, display + two framebuffers,
  the 3D scene buffers (`scene_init`), the audio mixer (`audio_mixer_init`),
  device-global settings (`se_hw_init`), the vsync/tearing-effect semaphore;
- **the frame loop** — per-frame delta-time (clamped), callback dispatch, the
  default backdrop clear, the blit at vsync, the double-buffer swap;
- **the input-queue pump** — it drains the BSP event queue each frame and
  consumes the device-global keys itself (volume ±, audio-jack re-route, and
  F1-exit when `cfg.f1_exits`), forwarding everything else to `on_input`.

The game supplies `se_app_callbacks_t`:

| Callback | When | Typical work |
|---|---|---|
| `on_init(user)` | once, after bootstrap | load world/save/content; read `se_display_info()` |
| `on_input(ev, user)` | per un-consumed event | latch edges (menu nav, buttons, typed chars) |
| `on_update(dt, user)` | every frame (**required**) | the state machine + simulation step |
| `on_backdrop(fb, user)` | every frame, first | draw the background; if NULL the engine clears to `cfg.backdrop_argb` |
| `on_render(fb, user)` | every frame, after backdrop | the 3D scene + HUD + menus |
| `on_shutdown(user)` | once, on `se_request_exit` | native-port cleanup (never fires under graceloader's F1-reboot) |

`user` is an opaque pointer threaded to every callback — your game state. (Race
the Synth passes `NULL` and uses file-scope state instead; both are fine.)

### Frame lifecycle

```
        ┌─────────────────────────── se_run frame loop ───────────────────────────┐
        │  pump input  ─▶ on_input* (per event)                                    │
        │  compute dt  ─▶ on_update(dt)        ── game state machine / physics     │
        │              ─▶ on_backdrop(fb)      ── or engine clears to backdrop_argb │
        │              ─▶ on_render(fb)        ── scene_begin → submit → scene_render│
        │  blit fb at vsync ─▶ swap buffers                                         │
        └───────────────────────────────────────────────────────────────────────────┘
```

The two framebuffers are the engine's; it hands the live back buffer to
`on_backdrop` / `on_render` as `fb`. A game that keeps many draw helpers can
mirror that pointer into a file-scope `fb` at the top of each callback (Race
the Synth does this) — but the buffer itself is engine-owned.

## Subsystems

Each subsystem is independent and has its own doc:

- **Renderer** (`se_scene`) — [renderer.md](renderer.md). World-space geometry
  in, projected + z-buffered pixels out.
- **Audio** (`se_audio` + sources + DSP) — [audio.md](audio.md). A mixer task
  on the I2S channel; the game implements music/SFX sources.
- **UI** (`se_ui`) + **bindings** (`se_bindings`) — [ui.md](ui.md).
- **Save** (`se_save` + `se_nbt`) — [save.md](save.md).
- **Device settings** (`se_hw`) — boot-apply + in-game get/set of the
  launcher-shared volume + brightnesses.
- **Vector text** (`se_text`) and **framebuffer leaves** (`se_direct565`) —
  the drawing primitives the renderer and UI build on.

They share nothing but the framebuffer and `se_config.h`; you can use the
renderer without audio, the save framework without the run loop, etc. The run
loop is the only thing that ties them together, and even it is optional (a
native port could drive the subsystems directly).

## Public vs internal

- **`include/` is the entire public API.** Every `se_*.h` there is semver'd
  (see [the version policy](../CHANGELOG.md)). `synthengine3d.h` is the umbrella
  and the table of contents.
- **`src/` (incl. `src/internal/`) is private.** Implementation files, the
  Hershey glyph tables, helper headers. A game must never include from `src/`;
  the build doesn't put it on a consumer's include path.

If you need something that's only in `src/`, that's a missing public API — file
it rather than reaching in.

## Performance contract

The engine targets a GPU-less ESP32-P4 doing software rasterization, so the
hot/cold split is explicit and **load-bearing**:

- **Hot per-pixel leaves are `static inline` in public headers** —
  `se_direct565.h` (pixel/line/triangle/dim-rect) and `se_text.h`'s Hershey
  stroker. They compile-fold the framebuffer's RGB565 format, the
  `PAX_O_ROT_CW` rotation and the stride into the inner loop. They **must stay
  inline** — moving one behind a function call or an opaque handle is a
  measurable regression. The extraction's regression gate was "identical
  compile flags ⇒ byte-identical codegen" for exactly this reason.
- **Coarse, once-or-few-per-frame work is ordinary (compiled, non-inline)
  functions** — the mixer, the save I/O, `se_menu_draw`, `scene_render`. These
  run off the per-pixel path, so a call boundary there costs nothing
  measurable and buys clean separation.

When you extend the engine, keep new code on the right side of that line: a new
per-pixel primitive goes inline in a header; a new subsystem entry point is a
normal function.

## How this engine was built

It was extracted, subsystem by subsystem, from a working game (Race the Synth)
rather than designed up-front — so every API is one that a real game needed.
The full phase-by-phase history (E0–EF + ER) lives in the game repo at
`../devdocs/engine-extraction.md`; you don't need it to *use* the engine, but
it's the rationale for why each boundary sits where it does.
