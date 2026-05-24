# Minimal example

The smallest complete app on SynthEngine3D: a spinning magenta triangle over a
flat backdrop, with procedural music. One file, [`main.c`](main.c).

It shows the whole shape of an engine app:

- **`app_main`** hands the run loop to `se_run(&cfg, &cb, NULL)` and never
  returns (F1 reboots to the launcher).
- **`on_init`** starts music — the engine has already booted the mixer, so this
  is one call.
- **`on_update(dt)`** advances state (the spin angle).
- **`on_render(fb)`** draws the 3D scene: `scene_begin` → `scene_tri` →
  `scene_render`. The engine cleared the backdrop before this.

Everything else — device bootstrap, the frame loop, vsync + blit, the input
pump, F1-exit — is the engine's.

## Building it

This file is **illustrative** and isn't compiled by the host game's build. To
run it on a Tanmatsu, make it your app's source and wire the engine in per
[`../../docs/integration.md`](../../docs/integration.md) (either build mode).
Sketch, plain-CMake mode:

```cmake
add_subdirectory(synthengine3D)
add_library(app_obj OBJECT examples/minimal/main.c)
target_link_libraries(app_obj PRIVATE synthengine3d)
# + fold $<TARGET_OBJECTS:synthengine3d> and app_obj into your app.so link
```

## Next steps

- Add a menu, a save file, input bindings → [`../../docs/getting-started.md`](../../docs/getting-started.md).
- Understand the run loop + callbacks → [`../../docs/architecture.md`](../../docs/architecture.md).
- Submit real models, not one triangle → [`../../docs/objects.md`](../../docs/objects.md).
