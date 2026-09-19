# Changelog

All notable changes to SynthEngine3D's **public API** (everything under
`include/`). The format follows [Keep a Changelog](https://keepachangelog.com/).
Versions are two-part, **MAJOR.MINOR**, as defined in `se_version.h`:

- **MAJOR** — a game has to change: changed signatures or semantics, removed
  symbols, changed public struct layout.
- **MINOR** — everything else: backwards-compatible additions, and internal
  changes (optimisation, refactor, bugfix) with no public-API effect.

Up to 1.1.0 there was also a PATCH number; 2.0 dropped it (see below).
`src/` (including `src/internal/`) stays internal and may change in any
release.

## [2.0] — unreleased (branch V2.0)

### Migrating from 1.x

- **`scene_tri()` takes a new last argument, `uint32_t flags`.** Append `, 0`
  to every call for exactly the old behaviour.
- **`SE_VERSION_PATCH` is gone**, and `se_version_string()` returns `"2.0"`,
  not `"2.0.0"`. Nothing else was renamed or removed.

### Changed — versioning

- **Two-part versions.** The patch number existed to signal "internal fix,
  safe to take". Games pin the engine by submodule commit, so the commit log
  already says that, and the number bought nothing. Internal fixes are now
  MINOR releases.

### Changed — `se_scene.h` (breaking)

- **`scene_tri(x0..z2, argb, flags)`.** A flag word rather than a boolean, so
  later per-triangle options fit without changing the signature again.
  Undefined bits are reserved and must be 0.
- **`se_geometry_t` gained `ttris` / `ttri_n` and `pts` / `pt_n`** (appended),
  so custom renderers can see textured triangles and points.
- **Near-plane clipping.** A triangle crossing `RENDER_NEAR_CLIP_Z` is now
  clipped to it (one or two triangles, texture coordinates interpolated,
  lighting of the original face kept) instead of having its behind-plane
  vertices clamped onto the plane, which distorted it. An edge crossing the
  plane is shortened to it. Primitives entirely in front of the plane are
  unaffected, bit for bit. A clipped triangle can take two list entries.
- **The depth scale follows `RENDER_NEAR_CLIP_Z`.** 1/z is stored as
  64000 × near / z, so the nearest drawable point encodes to 64000 whatever the
  near plane is. Before, the scale was fixed at 32000, which only fitted the
  default 0.5. A game overriding the near plane below ~0.49 overflowed the
  16-bit depth, so its nearest surfaces wrapped around and lost the depth test.
  At the default near plane nothing changes, bit for bit.

### Added — `se_scene.h` (triangle flags)

- **`SE_TRI_EMISSIVE`** — the triangle is never lit and keeps its colour (or
  texels) at full strength. For flames, lamps and screens, and for geometry the
  game shaded itself. The engine's splash now submits with it, so a light set
  before `se_splash()` no longer shades it twice.

### Added — `se_light.h` (scene lighting)

- **One optional positional light:** `se_light_set()` / `se_light_get()`.
  `brightness` is the directional share of the total illumination
  (`shade = (1 − brightness) + brightness · d`). Applied per face **at submit
  time** from the world-space normal. `two_sided` makes it independent of
  winding. No shadows, falloff or specular. Off until set, and then costs a
  load and a branch per triangle.

### Added — `se_scene.h` (points)

- **`scene_point(x, y, z, argb)`** — a single world-space pixel, e.g. for a
  starfield: projected with the camera, unlit, depth-tested against the
  triangles but never written to depth, drawn after the edges. Culled at
  submit time (behind the near plane, outside the viewport).
- **`se_pt_t`**, **`se_scene_raster_points()`** (for custom renderers; the
  built-in ones call it last), **`scene_point_stats()`**.
- **`SE_SCENE_POINT_CAP`** (`se_config.h`, default 1024): the list is
  allocated in PSRAM on the first `scene_point()`, so games that never call it
  pay nothing.

### Added — `se_scene.h` (quarter-resolution rendering)

- **`scene_set_render_scale(2)`** renders at half the width and height: a
  quarter of the pixels, so about a quarter of the rasterizing for a
  fill-bound scene. The camera, the `RENDER_*` projection, the viewport (still
  in full-screen pixels), culling, clipping and lighting are unchanged; only
  the projected positions are halved, so target pixel (i, j) is what full
  resolution draws at (2i, 2j). `scene_begin()` then takes a half-size buffer
  (e.g. an `se_ppa_layer_t`), which the game scales up. Latched per frame, so
  scenes can switch freely. Lines and points stay one target pixel wide. The
  raycast renderer falls back to the z-buffer at quarter resolution.
  `scene_render_scale()`; `se_geometry_t.scale` (appended). At scale 1 the
  output is bit for bit unchanged.

### Added — `se_ppa.h`

- **`se_ppa_blit_scaled(fb, id, layer, factor)`** — the whole layer scaled up
  by an integer factor (the quarter-resolution upscale). The PPA's scaler
  interpolates, so the result is soft rather than blocky.
- **`se_ppa_layer_sync(layer)`** — write back and invalidate a layer the CPU
  draws into every frame and the PPA reads or fills.
- **`se_ppa_buf_invalidate(buf)`** — drop a buffer the PPA wrote from the CPU
  cache before the CPU reads it.

### Added — `se_texture.h` and textured triangles

- **`se_texture_load(path, flags)` / `se_texture_unload(tex)`** — PNG via
  libspng into RGB565. Power-of-two edges up to `SE_TEXTURE_MAX_DIM`; alpha
  reduced to one bit (see cut-out transparency below). `SE_TEXTURE_INTERNAL` puts the texels in internal SRAM, with a
  logged PSRAM fallback reported in `tex->internal`.
- **`scene_textured_tri(v[3], tex, flags)`** — perspective-correct,
  nearest-texel, repeating, lit like `scene_tri`. Drawn from a list of its own
  (`SE_SCENE_TEXTURED_TRI_CAP`, allocated on the first texture load) after the
  flat triangles and before the edges, depth-tested against both. Both built-in
  renderers draw it. Custom renderers can call `se_scene_raster_textured()`.
- **Cut-out transparency:** a texel whose PNG alpha is below 128 is a hole
  (`SE_TEXEL_CUTOUT`) and draws neither colour nor depth, so what is behind
  shows through, with no sorting needed. `se_texture_t.cutout` (appended) tells
  whether a texture has any; `mean_argb` averages the opaque texels. Opaque
  textures render bit for bit as before. No blending.
- **`scene_textured_stats()`** — count and wallclock of the textured pass.
  `scene_raster_stats()` keeps meaning flat triangles only.

### Added — `se_run.h`

- **`se_present_stats(&blit_us, &vsync_us)`** — the present split into the
  LCD blit and the wait for the tearing-effect signal, so a game can tell a
  slow transfer from a frame that missed its refresh window.

## [1.1.0] — 2026-09-10

### Added — `se_scene.h` (viewport)

- **`scene_set_viewport(vp)` / `scene_viewport()`** restrict every pixel the
  scene writes to a rectangle. Triangles, wireframe edges and the depth plane
  are all clipped to it, and the optional frustum-cull pass now culls against
  the viewport rather than the whole screen, so a smaller viewport tightens
  culling for free. Both built-in renderers honour it: the z-buffer through its
  column/span bounds, the raycaster through its tile binning and its per-tile
  pixel loop.

  For a game that frames its 3D view inside a fixed border — a cockpit
  surround, a letterbox, a dashboard along the bottom. Without it, drawing the
  whole screen and then painting the border over the top pays the fill twice:
  once to rasterize pixels nobody sees, again to cover them. Those are usually
  the expensive pixels, since a dashboard sits over the nearest and most
  overdrawn band of a ground-plane scene.

  The viewport clips; it does not scale or re-centre. The projection is still
  the `RENDER_*` pinhole about `RENDER_HALF_W` / `RENDER_HORIZON_Y`, so an
  off-centre viewport shows an off-centre crop of the same image. Moving the
  vanishing point is what the `RENDER_*` overrides in `se_config.h` are for.

  Defaults to the whole framebuffer and is **not** reset by `scene_begin()` —
  how a game frames its view is a property of the game, not of the frame. A
  game that never calls it renders exactly as it did under 1.0.0.

## [1.0.0] — 2026-09-09

> **First stable release.** The public surface under `include/` is now frozen
> under semver: breaking changes to it require a MAJOR bump. Everything under
> `src/` (including `src/internal/`) remains internal and may change at any
> patch release.

### Added — `se_mp3.h` (MP3 music source)

- **`se_mp3_create(cfg)`** returns a `music_source_t` that plays `*.mp3` from
  a directory (default `/sd/music`), so a game can offer the player's own
  music as an alternative to the procedural generator by handing it to the
  same `audio_mixer_set_music()`. The mixer neither knows nor cares which
  source it holds. Plus `se_mp3_track_count()`, `se_mp3_track_name()`,
  `se_mp3_skip()`, and `se_mp3_config_t` (directory, shuffle, loop).
- Decoding is **minimp3** (public domain), vendored at `src/internal/minimp3.h`.
- **How it satisfies the mixer contract.** `se_audio_source.h` forbids blocking
  in `render()` — no file I/O, no waits — and fixes the format at 22050 Hz
  stereo; an MP3 is on the SD card and usually 44.1 kHz, so it meets neither.
  A decoder task therefore reads, decodes, resamples and fills a lock-free
  SPSC ring buffer, and `render()` only drains it — a bounded copy that never
  touches the filesystem. If the decoder falls behind, `render()` emits
  silence for the shortfall rather than stalling the mixer.
- Returns NULL (having logged why) when the directory is missing or empty, so
  a game can fall back to its existing music instead of going silent.
- **Not free**, unlike a procedural source: a 32 KB decoder-task stack
  (minimp3 is stack-hungry), a ~64 KB PCM ring and 16 KB read buffer in PSRAM,
  the `mp3dec_t` state in internal SRAM (it is touched per frame), and the CPU
  to decode. The task is pinned to the mixer's core below the mixer's
  priority, so it yields to audio.


### Added — `se_splash.h` (3D engine splash screen)

- **`se_splash()` / `se_splash_ex(title, subtitle, seconds)`** — a short 3D
  title sequence the engine draws for itself. Blocking: it runs its own frame
  loop, drawing and presenting until the animation ends (a ~1 s zoom then a
  2 s hold by default),
  holds for two seconds, then returns. Call it from `on_init()`.
- The wordmark is **real geometry**, not a scaled image: each Hershey glyph is
  walked stroke by stroke and emitted as world-space `scene_line()` segments on
  one z plane, which then flies from far to near through the engine's own
  pinhole camera. The zoom is therefore a true perspective approach — the text
  grows *and* spreads outward from the vanishing point — rather than a blit
  stretch. Font Y already points up, which is world +y, so glyph vertices need
  no flip (unlike the 2D text path).
- The default subtitle is `"Version <se_version_string()>"`, so it tracks the
  engine instead of going stale; pass your own to override.
- Requires `se_run()` to have bootstrapped (it borrows the engine's
  framebuffers and vsync). Called before that, it logs a warning and returns.
- Costs nothing if unused — `--gc-sections` strips it, as verified by Race the
  Synth's binary being byte-identical in size across this change.

### Added — pluggable renderers + `SE_RENDER_RAYCAST`

- **`scene_render()` / `scene_prepare()` / `scene_rasterize()` now dispatch on
  their `mode` argument** (it was accepted and ignored). Existing calls passing
  `SE_RENDER_ZBUFFER` are unaffected — same renderer, same output.
- **`SE_RENDER_RAYCAST`** — a second built-in renderer. Bins the projected
  triangles into 16x16 screen tiles, then casts one primary ray per pixel of a
  non-empty tile and writes each pixel **at most once**. A ray that hits
  nothing writes neither colour nor depth, so an existing backdrop shows
  through untouched, exactly as with the rasterizer.

  It renders the **same image** as `SE_RENDER_ZBUFFER`: for primary rays
  through a pinhole camera, "the ray through pixel p hits triangle T first" is
  precisely "p is inside T's projection and T holds the largest 1/z there" —
  the depth test. It therefore needs no world-space geometry and casts against
  the already-projected `(sx, sy, w)` triangles.

  The two differ only in cost, and in opposite directions: the z-buffer is
  primitive-driven and pays per covered pixel **per triangle** (overdraw); the
  raycaster is pixel-driven and pays per pixel of a **non-empty tile**,
  independent of depth complexity. Sparse scenes favour the z-buffer, dense
  high-overdraw scenes favour the raycaster. **Measure — do not assume:**
  Race the Synth's normal play (~400 triangles over 800x480) is sparse, and
  measures 12.6-22.4 ms z-buffer vs 60.9 ms raycast.
- **`se_renderer_register()` / `se_renderer_t` / `se_renderer_name()`** — a game
  can register its own renderer and select it exactly like a built-in, without
  changing any `scene_tri` / `scene_line` call site. The engine's cull / order
  passes run before a custom renderer's `prepare()`, so it inherits them.
- **`se_scene_geometry()`** and the now-public `se_vtx_t` / `se_tri_t` /
  `se_seg_t` — the per-frame projected geometry and render targets a custom
  renderer needs. (These structs were previously private to `se_scene.c`;
  publishing them is an addition, not a change — no existing field moved.)

### Changed (breaking — landed before the 1.0 freeze)
- **`render_camera_t` is now a full 6-DOF pose:** `{ x, y, z, yaw, pitch,
  roll }` (was `{ x, y }`). The two leading fields are unchanged, so code
  that reads `cam.x` / `cam.y` is source-compatible; the struct layout grew,
  hence a (pre-1.0) breaking bump. At zero `z` / orientation the projection
  is **byte-for-byte identical** to the old fixed pinhole.
- **`se_music_config_t` now carries a pluggable voice per role.** The separate
  per-voice `*_amp` gains, `*_env` (`se_music_env_t`) and `*_lpf`/`*_bpf`/`*_hpf`
  (`se_music_filter_t`) fields are replaced by one `se_voice_spec_t` per role
  (`bass`, `arp`, `pad`, `kick`, `snare`, `hat`) — gain/env/filter now live in
  the spec — plus optional per-role `*_voice` overrides and `pad_detune`
  (`pad_lfo_hz` moved into the pad spec's `amp_lfo_hz`). `se_music_env_t` /
  `se_music_filter_t` are removed (superseded by `se_env_t` and the spec).
  Games that only use `music_procedural_create(NULL, …)` (the preset) are
  unaffected. The synthwave sound is preserved.

### Added — `se_voice.h` (pluggable synth voices)
- **`se_voice_t`** — the voice interface (vtable): `note_on(freq, velocity)` /
  `note_off()` / `render(mix, frames)` (adds into a mono accumulator) /
  `active()`. One voice = one note; chords/polyphony are multiple voices. This
  is the unit the procedural sequencer triggers and the unit a **future MIDI
  player** will allocate per note — a game can also implement it for fully
  custom synthesis.
- **`se_voice_synth_t` + `se_voice_synth_init()`** — a built-in, embeddable
  (no-heap) subtractive/noise voice driven by **`se_voice_spec_t`**: oscillator
  (`se_osc_kind_t`: sine/saw/square/triangle/noise) ×1..`SE_VOICE_MAX_OSC`
  detuned → optional filter (`se_filter_kind_t`) → ADSR (`se_env_t`) → gain,
  with optional pitch-envelope and amplitude-LFO modulation. Covers the whole
  synthwave palette and is what the per-role specs build.

### Added — `se_ppa_blit_rect()` (PPA sprite blit)
- **`se_ppa_blit_rect(fb, job_id, layer, src_x, src_y, w, h, dst_x, dst_y)`** — a 1:1
  blit of an arbitrary `w×h` logical sub-rectangle of a layer to `(dst_x,dst_y)`,
  generalising the full-width-band `se_ppa_blit`. Lets a layer be sized to its
  artwork's bounding box (less SRM read/write, smaller cache) and lets callers
  clip a sprite to the screen/horizon by trimming `w`/`h` (non-positive = no-op
  success). The orientation transform (`band_to_raw`) was generalised to a
  logical rect (`rect_to_raw`). `se_ppa_blit` is unchanged.

### Added — `se_ppa.h` (ESP32-P4 PPA blit helper)
- **A generic PPA (Pixel-Processing-Accelerator) compositor** for offloading 2D
  blit work — backdrops, sprite layers, band fills — off the CPU. It owns the
  driver mechanics every app re-writes: the FILL / SRM / BLEND client lifecycle,
  an **ordered job queue driven by a pump task** (submits are non-blocking
  *enqueues* tagged with a caller `job_id`; a dedicated task submits them to the
  hardware one at a time in submission order, so execution order across op types
  is guaranteed with no cross-client races and no caller-managed waits — and the
  unsafe ISR-context submit is avoided, since the pump runs in task context),
  the **logical→raw orientation maths** (logical screen bands/rects → raw PPA
  picture-blocks — `PAX_O_UPRIGHT` + `PAX_O_ROT_CW` verified), and cache-line-
  aligned **PSRAM layer caches** with the one-shot C2M flush PPA's DMA needs.
- **API:** `se_ppa_init` · `se_ppa_layer_alloc` / `_flush` / `_free` ·
  `se_ppa_fill` / `_blit` / `_blit_rect` / `_blend_key` (non-blocking enqueues,
  each taking `job_id` as the 2nd arg, returning `bool` — `false` = refused) ·
  `se_ppa_wait_job` (drain to a job id) / `_wait_all` / `_pending`. New
  `se_ppa_layer_t`. Knobs in `se_config.h`: `SE_PPA_QUEUE_DEPTH`,
  `SE_PPA_CLIENT_QUEUE_DEPTH`, `SE_PPA_PUMP_TASK_PRIO` / `_STACK` / `_CORE`,
  `SE_PPA_CACHE_LINE`.
- **What stays with the caller:** the artwork, the band layout, and which CPU
  work overlaps the hardware (enqueue a batch, do CPU work, `se_ppa_wait_job`
  the id you need). Ordering is the engine's. ESP32-P4 only (the IDF component
  `REQUIRES … esp_driver_ppa esp_mm`); degrades to a logged no-op if init fails.
  Docs: `docs/ppa.md` + `examples/backdrop/`.

### Changed (internal)
- The procedural generator's six hardcoded voices became `se_voice_t`s driven
  through the note-on/off interface (the pad is now three voices — a chord —
  rather than one three-oscillator block; sonically equivalent by filter
  linearity, with the pad gain split across the three).
- **Scene rasterizer: the depth buffer and per-pixel frame-stamp plane are
  folded into one `uint32` cell** (`stamp << 16 | depth`). Halves the distinct
  cache lines the per-pixel depth test touches (one combined array + the
  framebuffer instead of separate depth and stamp planes) — a win on the
  PSRAM-latency-bound rasterize hot loop. Output is byte-identical; the stamp
  is now 16-bit (wraps every 65536 frames instead of 256). +~0.4 MB PSRAM for
  the wider cell.

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
- **`scene_prepare()` / `scene_rasterize()`** — the two halves of
  `scene_render()`, split so a game can overlap the geometry-only work
  (cull + order, no framebuffer access) with concurrent framebuffer activity
  such as a PPA backdrop blit, then rasterize once that completes.
  `scene_render()` is unchanged (it now calls the two in sequence); the output
  is identical.

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
