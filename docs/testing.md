# Testing a game off the badge — the host harness

A game's scene and asset code is ordinary C: it sets a camera, calls
`scene_tri()` / `scene_textured_tri()` / `scene_line()` / `scene_point()`, loads
textures. None of that needs an ESP32-P4 to *run* — only to be *drawn*. The host
harness in `host/` compiles that same code with a plain host compiler and hands
every primitive to a checker you write, so a test can answer questions about
what a frame contains:

- does anything cross the near plane (the artefact you see as geometry tearing
  open in front of the camera)?
- would a frame overflow `SE_SCENE_TRI_CAP`, `SE_SCENE_LINE_CAP` or
  `SE_SCENE_TEXTURED_TRI_CAP` and silently lose primitives on the badge?
- do two objects that should not touch end up intersecting?
- is the thing the shot is about actually on screen, and how big?

It runs in a second per scene, needs no device, and is the cheap half of a test
loop whose expensive half (what the frame *looks* like) belongs on the badge.

**Not simulated:** rasterizing, the depth buffer, PPA, audio, input, NVS, the
run loop, timing. This is about frame *content*.

---

## What you get

| File | What |
|---|---|
| `host/se_host.h` | The contract: the five `se_host_*` hooks your checker implements, and the camera helpers it can ask. |
| `host/se_host_stub.c` | The engine API implemented for a host: the badge's camera basis and projection, a remembering `se_light`, textures that always load (blank 64×64, so textured paths are taken), primitives forwarded to the hooks. |
| `host/shims/` | Stand-ins for `esp_log.h`, `esp_heap_caps.h`, `pax_gfx.h` and the `synthengine3d.h` umbrella, so the engine's public headers compile on a host. The umbrella includes the **real** `se_config.h` / `se_light.h` / `se_scene.h` / `se_texture.h`, so every type stays the engine's own. |
| `host/se_host_selftest.c` | The smallest working checker, and the harness's own regression test: `make -C host check`. Start your checker from it. |

## Building a checker

Three include paths, in this order, and link `se_host_stub.c`:

```sh
cc -O2 -Wall -Wextra \
   -Isynthengine3D/host/shims \   # MUST precede include/: the host umbrella wins
   -Isynthengine3D/host \
   -Isynthengine3D/include \
   -Imain \                       # your game's own headers
   my_checker.c my_scenes.c ... synthengine3D/host/se_host_stub.c -lm -o build/checker
```

Pass the game's compile-time engine settings here too, exactly as the app build
does — `-DSE_SCENE_TEXTURED_TRI_CAP=2048` and friends — or the checker will
compare frames against the wrong caps. Deriving both from one place (the app
`CMakeLists.txt`) is worth the five lines of Make.

## The hooks

```c
void se_host_tri(float const xyz[9], bool textured, uint32_t argb, uint32_t flags);
void se_host_line(float const a[3], float const b[3], uint32_t argb);
void se_host_point(float const p[3], uint32_t argb);
```

World space, submission order, before any clipping — `xyz` is three vertices.
A textured triangle arrives with `textured` true, `argb` its texture's
`mean_argb`, and its texture coordinates dropped. `flags` is what the game
passed (`SE_TRI_EMISSIVE`, …). All are required; the linker insists.

```c
void se_host_to_camera(float const world[3], float out_cam[3]);
void se_host_project(float const cam[3], float* out_sx, float* out_sy);
```

The same basis (`M = Ry(yaw)·Rx(pitch)·Rz(roll)`) and pinhole projection the
badge uses. `se_host_project` clamps z away from zero exactly as the engine
does, so test `cam[2]` against `RENDER_NEAR_CLIP_Z` yourself when it matters.

## Naming what you find

A checker that only counts is of little use — a finding must name the object.
The trick the showreel uses: geometry reaches the engine through the game's own
submission helper (`mesh_submit(mesh, xform, mats, n)`), so compile that helper
twice — once renamed — and wrap it:

```make
CFLAGS += -Dmesh_submit=mesh_submit_real
```

```c
void mesh_submit(mesh_t const* m, xform_t const* xf, mesh_mat_t const* mats, int n) {
    s_label = m->name;          // whatever the checker wants to blame
    mesh_submit_real(m, xf, mats, n);
    s_label = NULL;
}
```

Every primitive that arrives between those two lines belongs to that object, so
the hooks can attribute it without the engine knowing anything about it.

## Driving the scenes

If a game's per-frame content is a pure function of time — the showreel's
`scene_def_t` contract, `submit(t)` with no hidden state — a checker is a loop:

```c
scene->init("textures");
scene->enter();
for (int f = 0; f * dt < scene->duration; f++) {
    float const t = f * dt;
    scene->camera(t);           // sets the engine camera
    frame_begin(t);             // your bookkeeping
    scene->submit(t);           // everything lands in the hooks
    frame_check(t);             // your findings
}
scene->shutdown();
```

That purity is what makes the whole thing worth having: the same `t` gives the
same frame here, on the badge, and in a video export — so a host finding is
reproducible on the device, and a device screenshot can be a regression
reference (hash the framebuffer, compare next time).

## The device half

The harness deliberately cannot tell you whether a frame *looks* right. Pair it
with a device test that plays a named scene at named instants and reports a hash
of each finished framebuffer; store the hashes and compare them on later runs.
The showreel's `main/devtest.c` + `tools/testrun.py` (and the template's test
kit, which is the same thing generalised) do exactly that, and the two halves
together are what makes a hands-free `make cycle` possible.
