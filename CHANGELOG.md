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

The game this engine is measured in was called **CraftMiner** until
2026-09-25 and is called **SynthMiner** now. Entries below name it the
new way throughout, including entries written before the rename: the
measurements are the same measurements, and a name nobody can look up
helps no one. Nothing in the engine changed for it.

## [2.3] — 2026-09-26

### Changed — `se_stream.h`, `cfg.audio` now actually carries audio

No public signature moved. What changed is that asking for audio gets
audio: `se_stream_start()` with `cfg.audio` set used to log "no audio in
this stream; video only" and degrade, because the plumbing was finished
and there was no encoder behind it.

There is now: **pdmp2**, an MPEG-1/2 Layer II encoder written for this,
in the public domain. `src/internal/pdmp2/PROVENANCE.md` records where
every number in it came from, and upstream is
`github.com/nullislandspace/public-domain-mp2-encoder`.

**Why write one.** Every MPEG audio encoder worth vendoring — shine,
twolame, LAME — is LGPL. That is workable for an application and awkward
for a library, and an `app.so` that ships as a single blob nobody can
relink is close to the worst case for LGPL section 6: the relinking
obligation is the one it is hardest to honour. Everything else this
engine vendors is permissive (minimp3 is CC0, TinyUSB is MIT), and now so
is all of it.

**MPEG-2 LSF Layer II at 22050 Hz**, the mixer's own rate, so nothing is
resampled between the speaker and the stream. LSF Layer II also has
exactly one bit allocation table, so there is no rate-dependent table
selection to get wrong. A Layer II frame is 1152 samples a channel at
every rate — worth stating because Layer III's LSF frame is 576, and
picking the wrong number does not fail loudly, it never lines up and the
audio quietly never starts. 128 kbit/s, on a link where the video beside
it is twenty times that.

The muxer's PMT now announces stream_type **0x04** (ISO/IEC 13818-3)
rather than 0x03, because what it carries is MPEG-2 LSF and the PMT
should not contradict the frame headers. Both map to the same decoder in
practice; this one is correct.

Measured on the host rather than assumed: the encoder is round-tripped
through a reference decoder at six rate/channel/bitrate combinations and
beats ffmpeg's own MP2 encoder on 23 of 24 signal cases, at a level ratio
of 1.0000. The full path — encoder into the muxer into a `.ts` — is
checked too: `mp2 / 0x0004, 22050 Hz, stereo, 128 kb/s`, tones back at
1001 Hz and 439 Hz in the correct channels. The encoder's ~35 KB working
set is in PSRAM (`src/internal/pdmp2_port.h`); internal SRAM is the
scarce thing on this badge and the encoder touches those buffers once per
52 ms frame.

Two things this cost that are worth not learning twice, both recorded at
length in the upstream PROVENANCE.md: the Layer II **syncword is 12 bits,
not 11** (with `ID = 1` both readings give the same first two bytes, so
the mistake builds a working MPEG-1 encoder and a broken MPEG-2 one), and
**the analysis window is not a design choice** — a pseudo-QMF bank only
reconstructs if the analysis prototype is the time reverse of the
decoder's synthesis prototype, so a well-designed window measured 16 dB
SNR and a measured one measures 64.

## [2.2] — 2026-09-25

### Added — `se_stream.h`, live A/V streaming to a PC

The game's screen out of the USB-C port as H.264 in MPEG-TS over UDP, which
OBS plays directly with no server in between. Four calls: start, offer a
frame, stop, read the statistics.

Ported from `tanmatsu-nfmtest-grace`, which was written to find out whether
this was possible at all and what it cost. What that project proved, this
packages: the CDC-NCM link, the muxer and the hardware encoder come across
as they were, and the frame source does not. There the frames came from a
test pattern drawn in its own task and taken at a fixed rate; here they come
from the game, at whatever rate the game runs.

**The colour conversion is in the caller, on purpose.** `se_stream_frame()`
runs the PPA and returns; encoder, muxer and USB belong to another task.
That split is not about balance, it is about ownership: the moment the
render callback returns, the engine flips pages and the frame is being drawn
into again, so the only safe place to read it is before that. A frame
offered while the encoder is still busy is dropped and counted, never waited
for — a stream that stutters beats a game that does.

**It takes the console with it.** The USB-C port has one PHY, so starting a
stream hands it from the USB-Serial-JTAG console to the OTG controller:
while it runs there is no console, no debug link and no log output. A game
must therefore be able to switch it off without one, and should not persist
the setting. Everything that can fail and be reported happens before the
link comes up, while there is still somewhere to report it.

**Audio is the engine's to give.** Because the mixer is already here
(`se_audio.h`), `cfg.audio` needs nothing from the game: no tap to hold, no
callback to register. The plumbing is in — the mixer offers every chunk it
writes, silence included, and the PTS counts samples rather than reading a
clock, so it cannot drift against itself — but the codec is not, and
`cfg.audio` reports honestly until it is. Every MPEG audio encoder worth
vendoring is LGPL, and everything vendored here so far is permissive
(minimp3 is public domain, TinyUSB MIT), so that is a licensing decision
rather than a technical one.

TinyUSB is vendored under `src/internal/tinyusb` and built **only in
plain-CMake mode**: under the IDF the host already has a USB stack, and two
in one binary is one too many.


A 2.1 game builds unchanged unless it selects `SE_RENDER_RAYCAST` (see
below). Still unreleased and still being worked on, so it collects
everything of this round rather than taking a number per change --
including a renderer that was added, measured and removed again within
it, which is why no version was spent on it.

### Removed — `SE_RENDER_BANDED`, added and measured away in the same version

A second built-in renderer that ran the z-buffer's own passes one vertical
band of columns at a time, each band copied into internal SRAM, drawn
against a 16-bit depth buffer there, and copied back, so the per-pixel work
never touched PSRAM. It drew the z-buffer's image pixel for pixel, proved
by a host check over 1000 random scenes (full and quarter resolution,
viewports, cull and order, lighting, tint, cut-out textures) that also
caught a deliberately broken copy.

It was removed because it lost where it mattered. Measured in SynthMiner
over a fixed 40-second flight across five biomes (`claudeplans/synthminer.md`,
G6):

| | fps | rasterize |
|---|---|---|
| Quarter resolution, z-buffer | **20.16** | **27.59 ms** |
| Quarter resolution, banded | 18.13 | 29.72 ms |
| Full resolution, z-buffer | 5.70 | 148.18 ms |
| Full resolution, banded | **8.08** | **92.06 ms** |

1.61x faster at full resolution, 8% slower at quarter. Full resolution is
unplayable either way, so the only number that decides anything is the
quarter-resolution one. The split is explained: at quarter resolution
`SE_SCENE_DEPTH16_INTERNAL` already puts the depth test in SRAM, which is
the larger half of what banding buys, leaving only the colour writes
against the cost of setting a triangle up once per band it spans.

Wider bands would cut that cost, but 64 columns needs two 60 KB contiguous
blocks of internal SRAM and the largest free block on a P4 is 37-38 KB,
with or without the depth plane freed — so 32 was the widest the hardware
allows and the gap could not be closed. For reference, that same depth
plane is worth 27.59 ms against 40.11 ms to the z-buffer, which is a far
better use of the same SRAM than band buffers.

`SE_RENDER_BANDED` and `SE_SCENE_BAND_W` are gone and
`SE_RENDER_BUILTIN_COUNT` is back to 1, so `se_renderer_register()` hands
out handles from 1. Removing a public symbol is MAJOR by the rules above;
like the raycaster below it is recorded here as a deliberate exception,
because 2.2 has never been released and the symbol never shipped in one.

**What stays** is the raster target: the raster passes draw through one
struct (colour, depth, index offset, clip rectangle, fill counters)
instead of the frame-level buffers. It was introduced to make a band a
target and is kept because it is what a second core would need.

### Changed — the flip drops the frame from the cache

The present now writes the finished frame back **and invalidates it**
before flipping. The engine used to rely on each frame's working set
evicting a framebuffer's cache lines before the buffer was drawn into
again — with depth in internal SRAM that is no longer certain, and a
stale line that a partial CPU write (a HUD glyph) lands in would write
old pixels back over the PPA's new backdrop.

### Removed — the raycast renderer (a deliberate exception to MAJOR)

`SE_RENDER_RAYCAST` and the tiled primary-ray renderer behind it are
gone; the z-buffer is the only built-in renderer. `SE_RENDER_BUILTIN_COUNT`
dropped from 2 to 1 with it, and is back at 2 now that `SE_RENDER_BANDED`
takes the slot (above), so registered handles still start at 2.

Removing a public symbol is a MAJOR change by the rules above. This one
is recorded under 2.2 on purpose: no game renders with the raycaster, and
the only reference, Race the Synth's debug key that toggles to it, goes
when that game next moves its engine. A 3.0 for one debug key would say
more than the change does.

Why remove it: no game ever chose it. The measurement it shipped with
(1.0.0, below) had Race the Synth at 60.9 ms raycast against 12.6-22.4 ms
z-buffer. It still had to be kept in step with every renderer change —
near clipping, the viewport,
textures, quarter resolution (where it just fell back to the z-buffer) —
and it would have had to follow the renderer work now planned (banded
rendering in internal SRAM), where it would only get in the way. About
300 lines and its bin buffers (a 32768-entry pool, allocated on first
use) go with it.

### Changed — the present flips pages instead of copying (needs graceloader 2.6.0)

No public call changes, but **a game built with this engine needs
graceloader 2.6.0 or later**, which provides the display callback it uses.

Until now the engine drew into two framebuffers of its own, and each
present copied the finished one, all 768 KB, into the display driver's
own buffer, then waited on the panel's tearing-effect line. The copy ran
on DMA2D, so it cost little CPU, but it moved 1.5 MB through PSRAM every
frame — the same bus the rasterizer waits on — and it could land while
the display was reading that buffer.

The engine now draws straight into the driver's buffers, three of them,
and a present only selects the finished one for the next refresh. Three,
not two: with two, every frame would wait for the refresh that frees
the other buffer, on average half a refresh (8 ms at 60 Hz), about 10%
of a SynthMiner frame. With three, the display reads one, one waits for
the refresh, and the game draws into the third, so a game slower than
60 Hz never waits. A faster one waits for the refresh before flipping,
rather than drawing frames that are never shown. PSRAM use is unchanged
(three buffers before as well: two of the engine's plus the driver's).

The refresh signal comes from graceloader's
`graceloader_display_register_callbacks()`. The display driver only
accepts interrupt callbacks in IRAM, and a game's code is in PSRAM.

`se_present_stats()` keeps its two figures, with new meanings:
`blit_us` is the flip (the cache write-back), `vsync_us` the wait for
the display to pick up the previous frame, before the flip. `vsync_us`
is 0 for a game slower than the refresh. The tearing-effect line is no
longer used.

`fb` still changes every frame; it now rotates through three buffers
instead of alternating between two. Read it from the callback's argument
(or `se_frame_back()`) every frame, as before.

### Added — a scene-wide colour tint

```c
void se_scene_set_tint(uint8_t rg, uint8_t b);   // 0..32, 32 = unchanged
```

Scales the red and green of every triangle by one factor and the blue by
another, for a game that wants the whole scene to take a cast. Being
underwater is the case it was written for: water swallows red first and
green next and leaves blue.

**Red and green share a factor**, and that is the mechanism showing
through rather than a shy API. The textured inner loop scales all three
RGB565 channels with ONE multiply, by spreading them into separate fields
of a 32-bit word. One multiply cannot scale fields by different amounts;
two can, and two is what this is. A third channel would want a third
multiply, and nothing has asked for it.

**It is free when it is off.** The tinted raster loops are siblings of
the plain ones -- the same pattern the cut-out texture path already uses
-- so an untinted scene runs the code it always ran. With a tint of
(32, 32) the two-multiply form was checked against the one-multiply form
over all 65536 texels at all 33 shade levels and is bit-identical.

While it is on, the textured path costs one extra multiply per pixel.
The flat path costs nothing at all: a flat triangle is shaded once, at
setup, so the tint folds into a colour that was being computed anyway.

### Added — per-class volume, and a way to keep the speaker awake

`se_audio.h` gains three calls:

```c
void audio_mixer_set_music_volume(uint8_t percent);         // 0..100
void audio_mixer_set_group_volume(uint8_t group, uint8_t percent);
void audio_mixer_keep_awake(bool on);
```

The two volumes are what a game puts behind a slider. They scale each
class on top of the compile-time balance in `se_config.h`, so the
existing `AUDIO_MUSIC_GAIN` / `AUDIO_SFX_GAIN` still set the default
mix and the player adjusts from there. Both start at 100. They are
**not** the device volume, which belongs to the launcher.

Note that a volume of 0 is not the same as gating a class off: a silent
source still counts as something playing, and so still holds the
speaker up. Use `_set_music_enabled` / `_set_group_enabled` for off.

`audio_mixer_keep_awake()` exists because the idle policy had a sharp
edge that cost a game real time to find. The mixer mutes the amplifier
and disables I2S a few tens of milliseconds after the last sound, and
powers back up when the next voice is registered — correct, and
invisible, until the sounds are SHORT. An amplifier's turn-on is not
instantaneous, so a 35 ms click registered against a cold amp is over
before the speaker is listening. The audio is mixed and written
perfectly and is simply never heard.

SynthMiner met this as "the tool sounds only play when the music is
on", which is exactly what it looks like from the outside: a game with
a music source installed keeps the mixer busy every chunk (a source
rendering silence still counts as active), so the amplifier never
sleeps and the effects are fine — until the player turns the music off.

With `keep_awake(true)` the mixer feeds silence rather than powering
down, and every one-shot is heard from its first sample. The cost is
the amplifier's idle draw, so a game turns it on while it is being
played. A game that installs a music source and leaves it there has
been paying that cost all along.

## [2.1] — 2026-09-20 (unreleased, still being worked on)

Additive only: a 2.0 game builds unchanged.

### Added — radio rows in the menu (2026-09-23)

`SE_MENU_VAL_RADIO` (`se_ui.h`), appended to `se_menu_val_t`, for a list where
exactly one row is chosen: the engine draws a ring at the value column, filled
on the row whose `checked` is set. A check box says a setting is on or off by
itself; a radio says this row is the one in force and picking another drops
it, and drawing one as the other misleads the player about what the list does.

Reuses the existing `checked` field, so `se_menu_row_t` is unchanged; a game
that does not use the new kind is unaffected. The dot's size is
`SE_UI_RADIO_R` (`se_config.h`, 0.26 of the row text height).

### Changed — text is UTF-8, and the font has more than ASCII (2026-09-23)

`rendertext_draw` and `rendertext_size` (`se_text.h`) read their strings as
UTF-8 instead of one byte per glyph, and draw what they find: ASCII from
`simplex` as before, plus Cyrillic, accented Latin, the European quotation
marks, both dashes and the ellipsis. A codepoint the font cannot draw comes out
as an empty box rather than disappearing, and one malformed byte costs one
character, never the rest of the string.

An ASCII-only game is unaffected — every byte below 0x80 draws exactly the
glyph, at exactly the advance, that it drew before, and `simplex` itself is
untouched and still public. A game that was passing Latin-1 bytes (where 0xE4
drew nothing and took 16 units of space) now gets one box per invalid byte;
such a string was already not saying what it meant.

The new glyphs are generated from Hershey's own database, vendored at
`tools/hershey/hershey.dat` with the script that reads it. The generator
refuses to run unless the ASCII it regenerates matches the committed table and
every letter of every alphabet it lists has a glyph, so support is per language
rather than per string. `tools/hershey/README.md` says how to add one.

The alphabets covered are the 32 SynthMiner ships: every language written in
Latin, Greek or Cyrillic that Europe uses, Turkish included. That needed
sixteen accents (the usual five, plus caron, breve, double acute, macron, dot
above, ogonek and comma below), Greek from Hershey's greek SIMPLEX face --
the same weight as the Latin, unlike the Cyrillic, which he only drew in
complex -- and a dozen letterforms nobody can compose: Ł, þ, ð, đ, ı, Є, Ґ,
Џ, Ћ, Ђ and the Serbian Љ and Њ, which are ligatures joined on their shared
upright.

Costs: about 5 KB of rodata for the new tables, and per character a bisection
over 83 entries only when the codepoint is not ASCII. ASCII takes one compare
and an index, as it always did.

### Added — a quarter-resolution depth plane in internal SRAM (2026-09-22)

`SE_SCENE_DEPTH16_INTERNAL` (`se_config.h`, default 0). When a game sets it,
frames drawn at `scene_set_render_scale(2)` depth-test against a plain 16-bit
plane in internal SRAM (188 KB, cleared at `scene_begin()`) instead of the
stamped PSRAM plane. It is allocated before the geometry lists, so the lists
that no longer fit fall back to PSRAM. In SynthMiner it cut the per-pixel cost
by about 30% (flat 178 -> 122 ns, textured ~200 -> ~150 ns). Full resolution
is unchanged.

`se_geometry_t` gains `depth16`, appended at the end: on such frames `depth`
is NULL and `depth16` holds the plane (no stamp, 0 = infinitely far). With the
option off, `depth16` is always NULL and nothing else changes.

### Changed — depth order is a key sort (2026-09-22)

`depth_order` no longer qsorts the triangle records. Each triangle gets a
32-bit key in internal SRAM (16-bit depth over its 16-bit index); the keys are
radix sorted and the records gathered once into a second buffer, which then
swaps places with the list. The order is the same to within 1 part in 128 of
the depth sum; ties never change the image. Costs 8 bytes of internal SRAM per
triangle of the larger list's cap, plus one more list-sized buffer in the list's own
memory; falls back to qsort without them. Far view prep time in SynthMiner:
17.3 -> 10.5 ms with the lists in PSRAM. The list caps must stay at or below 65536
(a static assert).

### Fixed — `scene_drop_stats()` missed most drops (2026-09-22)

A triangle arriving at a full list with no vertex behind the near plane -- by
far the common case -- was dropped by an early return that did not count it;
only drops during near-plane clipping were counted. So a game overflowing the
lists every frame saw a drop count of zero. Found in SynthMiner, where half
the nearby terrain was vanishing with no warning in the log. Both lists now
count every drop.

### Added — a light level from the game, per triangle (2026-09-22)

- **`SE_TRI_LIGHT(n)`**, a triangle flag carrying a light level 0..32 that
  multiplies whatever shade the triangle gets -- the `se_light` shade for a
  lit triangle, full strength for an emissive one. Flat and textured alike;
  for a textured face it scales the existing per-face shade factor, so it
  costs nothing per pixel. For light the engine cannot know: a torch in a
  cave, night falling on a block world. Stored as the darkness (32 - n) in
  bits 8..13, so flags written without it mean full light and nothing that
  exists changes. `se_tri_light_level()` reads it back.

### Added — bindings a game persists itself (2026-09-22)

- **`se_bindings_config_t.nvs_namespace` may be NULL.** The engine then keeps
  the bindings in memory only: `se_bindings_init` starts every control at its
  default and touches no NVS, and `se_bindings_set` updates the value without
  writing anything. The game saves and restores them (restoring with
  `se_bindings_set` after init). For a game whose settings live in a file on
  the SD card, beside its saves, so one copy backs up everything. A non-NULL
  namespace behaves exactly as before.

### Added — scrolling list menus (2026-09-22)

- **`se_menu_def_t.visible_rows`.** A menu with more rows than this shows a
  window of them that keeps the cursor in view, with a caret in the chevron
  gutter while there is more above or below. The window is worked out from the
  cursor on every draw, so there is no scroll state to keep: the cursor still
  counts over all rows and `se_menu_input` is unchanged. 0 (what a 2.0 game's
  initialisers leave it) draws every row, as before. The field is appended to
  the end of the struct, so every existing designated initialiser still builds.
  Written for SynthMiner's Controls menu, 23 rows in a panel that holds seven.

### Changed — `se_ui_capture_key()` binds the cursor keys (2026-09-22)

It refused every escaped scancode (0xE0xx) and mapped only F1-F12 from the
navigation channel, so the arrow keys -- the grey block generally -- could not
be captured at all. A game that shipped an arrow key as a default (SynthMiner's
look keys) could never have it bound back once a player changed it. Now the
escaped grey keys bind as their scancodes, and a keyboard that sends the cursor
keys, Home/End or Page Up/Down only as navigation events binds the same
scancode as one that sends them as scancodes. The "fake shift" codes some
keyboards wrap round the grey keys are still refused. No signature changes; a
game gets this by rebuilding, and a binding it already stores is unaffected.

### Changed — the rasterizer is faster, and says why (2026-09-21)

Measured in SynthMiner on the badge, over a 20-second flight across streamed
voxel terrain. Nothing about the public API changes except one addition below;
a game gets this by rebuilding.

- **Built with `-O2`, not `-Os`.** The engine runs from PSRAM, where code size
  is the one resource that is not scarce. At `-Os` the compiler would not inline
  `scene_index()` or the span functions despite their `static inline`, so every
  span paid a function call. **Flat fill 22.0 → 19.4 ms a frame, textured
  37.7 → 34.7 ms**; `.text` grew 7 KiB.

- **`ceilf`/`floorf` are gone from the column scans.** They are library calls
  even at `-O2` — they have to set `errno`, so the compiler cannot fold them
  into the one RISC-V convert instruction the value needs. The scans called them
  twice per span, and a voxel frame draws **28000 spans**: 57000 library calls a
  frame to round a number already sitting in a float register. `ceil_i()` /
  `floor_i()` do it inline, with identical results for every finite in-range
  value. **Flat fill 19.4 → 13.4 ms, textured 34.7 → 31.7 ms.**

Together: **rasterize 49.7 → 43.6 ms**, and the frame rate of the case that
motivated it went 13.4 → 15.3 fps.

The measurement that found this is worth more than the fix. A voxel scene's
spans average **6 pixels** (flat) and **12** (textured), so the *per-span* cost
— clip, plane setup, call — dominated the per-pixel one. That is also why
vectorising the inner loops (the ESP32-P4's PIE SIMD, which this toolchain
already enables) was **not** the answer: there is barely a vector's worth of
pixels in a span to begin with.

### Added — `scene_drop_stats()` (2026-09-21)

- **A full geometry list dropped silently, and now it counts.** `scene_tri()`
  and `scene_textured_tri()` return without drawing once the frame's list is
  full — the only sane thing a fixed list can do — but they did it without a
  word, and the drop is in *submission order*, so what disappears is whatever
  the game happened to submit last: a corner of the world, a chunk, half a
  title screen. That reads as a bug in the game, and it cost SynthMiner two
  debugging sessions before it was made visible.

  `scene_drop_stats(int* tris, int* ttris)` reports what this frame's lists had
  no room for. **Anything non-zero is a hole in the picture.** Counted from
  `scene_begin()`; read it after submitting.

### Added

- **`scene_fill_stats(tri_px, ttri_px, tri_spans, ttri_spans)`** — pixels
  covered and spans walked by the last rasterize, flat and textured. Counted per
  span, so they cost nothing. `*_us / *_px` is nanoseconds per pixel and
  `*_px / *_spans` the average run length: between them they say whether a fill
  loop is bound on its arithmetic, on memory, or on its own setup — which is not
  guessable from the outside, and was guessed wrong here twice before being
  measured.

### Added — host harness (`host/`)

- **Run a game's scene code on a PC.** `host/se_host_stub.c` implements the
  engine API a game's scene and asset code calls — the camera (the badge's own
  basis and projection), `se_light`, `se_texture_load` (always succeeds, blank
  64×64) and the four primitive calls — and forwards every primitive to five
  `se_host_*` hooks the game implements. `host/shims/` holds stand-ins for the
  ESP-IDF and PAX headers the public headers mention, plus a host
  `synthengine3d.h` that includes the real `se_*.h`, so the types stay the
  engine's. Nothing is rasterized: this checks what a frame *contains* (near-plane
  crossings, list overflows, object clearances, framing), in a second, with no
  device. `host/se_host_selftest.c` is the worked example and the regression
  test (`make -C host check`); [`docs/testing.md`](docs/testing.md) is the guide.
- It lived in the showreel before this, where it had copied the projection and
  the list caps out of the engine by hand and could drift from them silently.

### Added — `se_config.h` (list caps)

- **`SE_SCENE_TRI_CAP`** and **`SE_SCENE_LINE_CAP`** (both default 4096, the
  values they always had) are now public, overridable `#ifndef` macros like
  `SE_SCENE_TEXTURED_TRI_CAP` and `SE_SCENE_POINT_CAP` beside them. They were
  private to `se_scene.c`, so a game could not size its frame lists, and a
  host-side checker could only copy the numbers and hope they stayed true.

## [2.0] — 2026-09-19

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
