# Renderer (`se_scene.h`)

A per-pixel **z-buffered** software 3D renderer with a pinhole camera. Games
submit world-space triangles and wireframe edges; the engine projects,
depth-tests and rasterizes them. There is no GPU — this is all CPU/PSRAM on the
ESP32-P4 — so the design is shaped by being **fill-bound**.

## Coordinate system

World space is **x = lateral, y = up, z = forward** (into the screen). The
camera is a pinhole at z = 0, with a lateral position `x` and an eye height
`y`; it looks down +z. A world point projects as:

```
sx = RENDER_HALF_W    + RENDER_FOCAL_LEN * (x - cam.x) / z
sy = RENDER_HORIZON_Y - RENDER_FOCAL_LEN * (y - cam.y) / z
```

`RENDER_*` are overridable in [`se_config.h`](configuration.md) —
`RENDER_HORIZON_Y` in particular is the knob to line the 3D horizon up with
your backdrop. Set the camera once per frame **before** submitting:

```c
render_set_camera(float x, float y);
render_camera_t render_camera(void);                 // read it back
void render_project(x_w, y_w, z_w, &out_sx, &out_sy); // project a point yourself
```

`render_project` is for 2D work that must line up with the 3D scene (e.g.
drawing a ground shadow under a projected object).

## The frame: begin → submit → render

```c
scene_init();                       // once at boot (se_run does this for you)

scene_begin(fb);                    // per frame: bind fb, empty the z-buffer
scene_tri(...);  scene_line(...);   // submit world-space geometry, any order
scene_render(SE_RENDER_ZBUFFER);    // rasterize the whole frame, then reset
```

- **`scene_tri(x0..z2, argb)`** — a filled, flat-shaded, depth-tested triangle.
- **`scene_line(x0..z1, argb)`** — a wireframe edge, depth-tested with a small
  bias so it wins against the coplanar face it outlines but loses to nearer
  geometry. (Hershey text mapped onto 3D surfaces is just a fan of these — see
  [objects.md](objects.md).)
- **`scene_render(mode)`** — rasterizes the accumulated frame and empties the
  lists. `scene_flush()` is a back-compat alias for `scene_render(SE_RENDER_ZBUFFER)`.

Submission order is irrelevant: the per-pixel depth test resolves visibility,
so stacked, straddling and interpenetrating geometry all "just work" without
the game sorting anything.

### Deferred pipeline

`scene_tri` / `scene_line` do **not** draw on the call. They project with the
current camera and **accumulate** into per-frame triangle / edge lists;
`scene_render()` does all the rasterization at once. Holding the whole frame is
what lets the engine own the algorithm — and, in future, cull and order
centrally — without any game call site changing:

```
submit ─▶ [tri list] [edge list] ─▶ scene_render: cull ─▶ order ─▶ rasterize
                                                  (no-op) (no-op)  (z-buffer)
```

`se_render_mode_t` selects the algorithm; today only `SE_RENDER_ZBUFFER`
(= `SE_RENDER_DEFAULT`) ships. The **cull and order passes are currently
no-ops**: `scene_render` rasterizes in submission order with the z-buffer, so
output is identical to naive immediate-mode drawing. They exist as seams so a
later cut can add frustum/back-face culling and front-to-back ordering
(early-z rejection — the likely win on this fill-bound device) without touching
games. A whole-triangle near-clip guard stays in `scene_tri` (it bounds the
projection; it is not the central cull).

**Buffer caps.** The triangle and edge lists are fixed PSRAM buffers
(`SCENE_TRI_CAP` / `SCENE_LINE_CAP`, 4096 each); submitting past a cap silently
drops the extra geometry. A few thousand triangles is well within budget.

## Depth buffer

Depth is a scaled reciprocal-z (1/z), the quantity that interpolates linearly
in screen space — so the per-pixel inner loop is one add, no divide. Larger
encoded value = nearer.

The depth buffer is **never bulk-cleared**. A parallel 8-bit per-pixel "frame
stamp" records which frame last wrote each depth; a depth counts only if its
stamp is the current frame, so a stale pixel reads as infinitely far. That
makes `scene_begin()` a single counter increment instead of a full-screen
memset, and confines depth traffic to the pixels the scene actually touches.
(The stamp wraps every 256 frames; the one-frame, one-pixel mis-resolve that
could in principle cause is invisible in practice.)

## What the engine does and doesn't do

- **Does:** projection, the near-clip guard, per-pixel depth test + write,
  flat-shaded triangle fill, depth-biased wireframe, the camera.
- **Doesn't (yet / by design):** lighting, texturing, per-vertex colour,
  central culling/ordering (no-op seams). **The game owns shading** — compute
  a face colour (e.g. per-face lighting from a normal) and pass it as the
  triangle's `argb`. **The game owns its object/world model** — the engine
  never sees "objects", only triangles and edges (see [objects.md](objects.md)).

## Tuning notes

- It's fill-bound: cost scales with on-screen pixels, not triangle count per
  se. Low-poly models + culling invisible faces game-side is the lever today.
- The wireframe overlay hides the occasional 1-px seam from the non-sub-pixel
  triangle fill; a wireframe-free look would want a half-space rasteriser
  (not currently provided).
