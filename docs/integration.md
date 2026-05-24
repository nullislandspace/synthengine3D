# Integration

How to consume SynthEngine3D from a build, in either of its two modes, plus its
dependencies and what to change when porting.

## Dependencies

- **ESP-IDF** + the **Tanmatsu BSP** (`bsp/display.h`, `bsp/input.h`,
  `bsp/audio.h`, `bsp/led.h`, `bsp/device.h`) — the engine talks to the device
  through the BSP, and to FreeRTOS, NVS, `esp_timer`, `esp_heap_caps`.
- **PAX** (`pax_gfx.h`, `pax_text.h`) — the framebuffer type + a few helpers;
  the hot per-pixel paths are the engine's own `se_direct565` leaves, not PAX.
- **FreeRTOS** — the audio mixer + the PPA completion ISR run off the main
  task; the IDF component declares `REQUIRES freertos`.
- **PPA driver + `esp_cache`** (`esp_driver_ppa`, `esp_mm`) — the `se_ppa`
  hardware compositor ([ppa.md](ppa.md)). **ESP32-P4 only**; the IDF component
  adds `REQUIRES … esp_driver_ppa esp_mm` for them. A non-P4 port drops `se_ppa`
  (no other subsystem depends on it).

The engine includes **no game headers** — the boundary is one-directional. (You
can verify: nothing under `include/` or `src/` includes a game header.)

## Mode A — ESP-IDF component

Under `idf.py`, `synthengine3D/CMakeLists.txt` registers a normal IDF component
via `idf_component_register()` (`INCLUDE_DIRS include`, `PRIV_INCLUDE_DIRS src
src/internal`, `REQUIRES freertos esp_driver_ppa esp_mm`). A full IDF app (e.g.
graceloader itself) consumes it like any component:

- **Vendored:** drop `synthengine3D/` into your project's `components/` (or add
  its parent to `EXTRA_COMPONENT_DIRS`).
- **Managed:** `idf_component.yml` is present so it can later move to its own
  repo / the component registry and be pulled in as a dependency.

Then `#include "synthengine3d.h"` (or individual `se_*.h`) from your app.

## Mode B — plain-CMake object library

Under a hand-rolled `app.so` build with no `idf.py` (how Race the Synth builds),
the same `CMakeLists.txt` takes the `else()` branch: it defines a relocatable
**OBJECT library** `synthengine3d` whose objects the top-level link script folds
into the app's shared object. To wire it in:

```cmake
add_subdirectory(synthengine3D)
target_link_libraries(app_obj PRIVATE synthengine3d)   # propagates include/
# and add  $<TARGET_OBJECTS:synthengine3d>  to your link inputs + DEPENDS
```

**The compile environment must match your app's**, or the inline leaves won't
codegen identically. The object-library branch already mirrors the app's flags
(`-Os -fPIC -ffunction-sections -fdata-sections -fno-common`, the FreeRTOS
force-includes, `-DESP_PLATFORM`); the `xesppie` march / `ilp32f` ABI that carry
the PIE SIMD come from the toolchain file and apply project-wide, so they're
inherited automatically. If you change your app's flags, change the engine's to
match.

## What's on the include path

- **`include/`** — public, the only consumer-visible dir. Put it on your
  include path; `#include "synthengine3d.h"` for everything, or pick headers.
- **`src/` + `src/internal/`** — private; the build keeps them off the
  consumer path. Never include from there.

## Porting / first-run checklist

1. **Display:** set the `DISPLAY_*` macros in [`se_config.h`](configuration.md)
   for your panel (or override them); the engine resolves the actual format /
   orientation from the BSP at boot and you read it back via `se_display_info()`.
2. **Projection:** set `RENDER_HORIZON_Y` (+ focal length / cam height) to suit
   your scene and backdrop.
3. **Bootstrap:** call `se_run(&cfg, &cb, user)` from `app_main` with at least
   `cb.on_update`. The engine brings up display / audio / scene / settings
   itself — don't double-init them.
4. **Build green check:** the engine builds with `REQUIRES freertos
   esp_driver_ppa esp_mm`; if you see undefined BSP/PAX symbols, they're
   satisfied by your app's link
   against the platform libs (the engine declares against them, doesn't bundle
   them).

## Versioning

`se_version.h` carries `SE_VERSION_*` and `se_version_string()`. The public
surface (`include/`) is semver'd; see [`../CHANGELOG.md`](../CHANGELOG.md) and
the policy in `se_version.h`. Pre-1.0, pin to a known-good revision if you need
stability while the API settles.
