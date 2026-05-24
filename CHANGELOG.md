# Changelog

All notable changes to SynthEngine3D's **public API** (everything under
`include/`). The format follows [Keep a Changelog](https://keepachangelog.com/),
and the project follows semantic versioning as defined in `se_version.h`:

- **MAJOR** — incompatible public-API change (signatures, semantics, removed
  symbols, changed public struct layout).
- **MINOR** — backwards-compatible additions to the public API.
- **PATCH** — internal-only changes (optimisation, refactor, bugfix) with no
  public-API effect.

While the version is `0.x`, the surface is documented and semver-tracked but
not yet frozen — minor releases may still adjust the API as it settles toward
`1.0`.

## [Unreleased]

> Committed but not yet assigned a version number. These changes sit on top of
> 0.2.0 until a release version is chosen.

### Changed (breaking — allowed pre-1.0)
- **`render_camera_t` is now a full 6-DOF pose:** `{ x, y, z, yaw, pitch,
  roll }` (was `{ x, y }`). The two leading fields are unchanged, so code
  that reads `cam.x` / `cam.y` is source-compatible; the struct layout grew,
  hence a (pre-1.0) breaking bump. At zero `z` / orientation the projection
  is **byte-for-byte identical** to the old fixed pinhole.

### Added
- **`render_set_camera_6dof(x, y, z, yaw, pitch, roll)`** — position the eye
  anywhere and orient it (yaw about world-up, then pitch about right, then
  roll about forward; radians). `render_set_camera(x, y)` stays as the legacy
  shorthand (eye at `z = 0`, zero orientation). The rotation basis is cached
  per `set`, so the trig runs once per frame, not per vertex.
- **`se_scene_options_t`** + **`scene_set_options()` / `scene_get_options()`**
  — two opt-in, **output-neutral** render passes, toggled at runtime, both
  default OFF:
  - `frustum_cull` — drop geometry that projects entirely off-screen. Because
    it runs after projection, it respects the camera pose + FOV for free.
  - `depth_order` — front-to-back triangle sort for early-z; a win under heavy
    overdraw, measure under light overdraw.
  Back-face culling is intentionally NOT an engine pass — it belongs in the
  game's objects, which know their face normals (e.g. `emit_cube`).

### Resolved
- The ER first cut's no-op cull/order seams are now real, opt-in passes; the
  camera gained the deferred-noted "zoom / shake / look-ahead" headroom as
  full 6-DOF. Defaults keep `scene_render()` byte-identical to 0.2.0.

## [0.2.0] — 2026-05-24

### Changed (breaking — allowed pre-1.0)
- **`music_procedural_create()` gained a config parameter:**
  `music_procedural_create(const se_music_config_t* cfg, uint32_t seed)`.
  Pass `NULL` for the built-in synthwave preset (so the common case is a
  one-token change: `…create(seed)` → `…create(NULL, seed)`).

### Added
- **`se_music_config_t`** + supporting public types (`se_music_chord_t`,
  `se_music_progression_t`, `se_music_arp_pattern_t`, `se_music_drum_pattern_t`,
  `se_music_env_t`, `se_music_filter_t`) and the grid constants
  `SE_MUSIC_TICKS_PER_BAR` / `SE_MUSIC_CHORDS_PER_SECTION`. A game now drives
  the procedural generator with its own **content + tone** — tempo range,
  tonic pool, chord/arp/drum/bass pattern banks, per-layer gains, and each
  voice's envelope + filter — for genuinely different music (key, rhythm,
  harmony, balance, timbre) on the same six-voice synth.
- **`se_music_synthwave_preset()`** — the built-in synthwave personality as a
  config; what `NULL` selects.

### Resolved
- The 0.1.0 "procedural music content is hardcoded synthwave" limitation
  (planned as E2.1). The six-voice synth *topology* (saw bass / square arp /
  3-saw pad / sine kick / noise snare+hat) and the fixed 4/4 16th-note,
  8-chord-section grid remain shared structure; everything musical is now
  data. (A future change could make the synth voices pluggable too.)

## [0.1.0] — 2026-05-24

First documented release: the engine is feature-complete for its source game
and has a full doc suite. Extracted from **Race the Synth** across phases
E0–EF + ER (see `../devdocs/engine-extraction.md` for the extraction history).

### Added — public API surface
- **`se_run.h`** — application-framework run loop (`se_run`, `se_app_config_t`,
  `se_app_callbacks_t`, `se_request_exit`, `se_display_info`).
- **`se_scene.h`** — z-buffered 3D renderer + pinhole camera + projection
  (`scene_init/begin/tri/line/render/flush`, `se_render_mode_t`,
  `render_set_camera`/`render_camera`/`render_project`). Deferred pipeline
  (ER): `scene_tri`/`scene_line` accumulate; `scene_render(mode)` rasterizes.
- **`se_audio.h`** + **`se_audio_source.h`** — software mixer + the
  `music_source_t` / `sfx_voice_t` source contracts, app-pushed mute groups.
- **`se_audio_dsp.h`** — oscillator / envelope / biquad DSP primitives.
- **`se_music_procedural.h`** — seed-driven procedural music source.
- **`se_ui.h`** — data-driven list menus (`se_menu_def_t` / `se_menu_t`,
  `se_menu_input` / `se_menu_draw`, row kinds NONE/CHECK/TEXT/CUSTOM/RANGE)
  + the blocking `se_ui_capture_key` rebind modal.
- **`se_bindings.h`** — remappable, NVS-persisted key bindings.
- **`se_hw.h`** — device-global hardware settings (volume + 3 brightnesses):
  boot-apply, in-game get/set, persistence to the launcher-shared NVS.
- **`se_save.h`** + **`se_nbt.h`** — file-backed save slots with a peek header,
  over an NBT serialization primitive.
- **`se_text.h`** — Hershey vector text.
- **`se_direct565.h`** — inline RGB565 framebuffer primitives.
- **`se_config.h`** — overridable compile-time defaults (display, projection,
  audio gains, UI theme, save-slot count, bindings cap).
- **`synthengine3d.h`** — umbrella header.

### Known limitations / planned
- **Procedural music content is hardcoded synthwave.** A planned follow-up
  (E2.1) lifts instruments / scales / progressions into a public
  `se_music_config_t` passed to `music_procedural_create()`; that will add a
  config parameter to the seed-only signature (a MINOR change).
- **Renderer cull + order are no-op seams.** `scene_render()` rasterizes in
  submission order; central frustum/back-face culling and front-to-back
  ordering are stubbed (`scene_cull_pass` / `scene_order_pass`) for a later,
  measured cut. No public-API impact when they land.
- **No object framework.** Games own their object/world pool and submit naive
  world-space geometry via `scene_tri` / `scene_line` (see `docs/objects.md`).
