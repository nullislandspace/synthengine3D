# Renderer (`se_scene.h`)

A per-pixel **z-buffered** software 3D renderer with a pinhole camera. Games
submit world-space triangles and wireframe edges; the engine projects,
depth-tests and rasterizes them. There is no GPU — this is all CPU/PSRAM on the
ESP32-P4 — so the design is shaped by being **fill-bound**.

## Coordinate system

World space is **x = lateral, y = up, z = forward** (into the screen). The
camera is a **6-DOF pinhole**: an eye position `(x, y, z)` plus an orientation
`(yaw, pitch, roll)` in radians. At zero orientation it sits looking straight
down +z with +y up and +x right, and a world point projects as:

```
sx = RENDER_HALF_W    + RENDER_FOCAL_LEN * cx / cz
sy = RENDER_HORIZON_Y - RENDER_FOCAL_LEN * cy / cz
```

where `(cx, cy, cz)` is the world point expressed in camera space (translate by
the eye, then rotate by the pose). With `z = 0` and zero orientation this
reduces to `(x - cam.x, y - cam.y, z)` — i.e. **byte-for-byte the old fixed
pinhole**, so games written against the 2-axis camera are unchanged.

`RENDER_*` are overridable in [`se_config.h`](configuration.md) —
`RENDER_FOCAL_LEN` / `RENDER_HALF_W` set the FOV, and `RENDER_HORIZON_Y` is the
knob to line the 3D horizon up with your backdrop. Set the camera once per
frame **before** submitting:

```c
render_set_camera(float x, float y);                 // legacy: eye at z=0, no rotation
render_set_camera_6dof(x, y, z, yaw, pitch, roll);   // full pose (radians)
render_camera_t render_camera(void);                 // read it back
void render_project(x_w, y_w, z_w, &out_sx, &out_sy); // project a point yourself
```

The rotation basis is rebuilt inside the setter, so the trig runs once per
frame — the per-vertex transform is just a 3×3 multiply. `render_project` is for
2D work that must line up with the 3D scene (e.g. drawing a ground shadow under
a projected object); it uses the same pose.

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
what lets the engine own the algorithm and cull / order the geometry centrally
without any game call site changing:

```
submit ─▶ [tri list] [edge list] ─▶ scene_render: cull ─▶ order ─▶ rasterize
                                                  (opt-in)(opt-in) (z-buffer)
```

`se_render_mode_t` selects the algorithm; today only `SE_RENDER_ZBUFFER`
(= `SE_RENDER_DEFAULT`) ships. A whole-triangle near-clip guard stays in
`scene_tri` (it bounds the projection; it is not the central cull).

### Two-phase render (`scene_prepare` / `scene_rasterize`)

`scene_render()` is two halves you can also call separately, to overlap the
geometry-only work with concurrent framebuffer activity:

```c
scene_prepare(SE_RENDER_ZBUFFER);     // cull + order — touches NO framebuffer
// ... kick/await other framebuffer work here (e.g. a hardware blit) ...
scene_rasterize(SE_RENDER_ZBUFFER);   // paint the prepared geometry
```

`scene_prepare()` runs the cull and order passes over the deferred lists and
touches **no framebuffer pixels** — only the geometry lists — so it is safe to
run *concurrently* with a hardware block writing the framebuffer, e.g. a PPA
backdrop composite (see [ppa.md](ppa.md)). `scene_rasterize()` then does the
actual pixel fill and must run **after** that blit completes (near geometry
projects up into the backdrop region, so it overwrites those pixels). The order
is: submit all geometry → `scene_prepare()` → kick/await the concurrent
framebuffer work → `scene_rasterize()`.

`scene_render()` is exactly these two back-to-back, and the output is identical
either way — use it whenever there's nothing to overlap. The source game uses
the split: it prepares the scene *during* the PPA sky/sun/mountain DMA (the
CPU-side transform + cull runs while the blit drives the PSRAM bus), then
rasterizes once the backdrop is down. For that overlap to be real and not just
bus contention, keep the geometry lists off PSRAM — see **Buffer caps** below.

### Optional passes (`scene_set_options`)

The cull and order passes are **opt-in and default OFF**, so out of the box
`scene_render` rasterizes in submission order — byte-identical to naive
immediate-mode drawing. Both are **output-neutral**: they change only how fast
the frame draws, never the pixels, so they are safe to toggle live.

```c
scene_set_options(&(se_scene_options_t){ .frustum_cull = true, .depth_order = false });
se_scene_options_t o = scene_get_options();   // get-modify-set to flip one
```

- **`frustum_cull`** — drops triangles/edges that project entirely off-screen
  before rasterizing (cheap O(n) screen-space bounds test). Because it runs
  *after* projection, the screen rectangle **is** the projected view frustum,
  so it respects the camera pose and FOV for free — no frustum-plane math, and
  it stays correct if you later move or rotate the camera. A near-pure win
  whenever the world submits geometry outside the view.
- **`depth_order`** — sorts triangles front-to-back so occluded fragments fail
  the depth test with no framebuffer write (early-z). Costs an O(n log n) sort
  per frame: a win under heavy overdraw (dense scenes), can lose under light
  overdraw — measure it. Edges are never sorted (they don't write depth).

**Recommended starting point: `frustum_cull` on, `depth_order` off.** In the
source game's on-device A/B, frustum cull was a strict win in every scene
(~6% off the scene-render time), while depth order only paid off under heavy
overdraw and *cost* a few percent in sparse scenes (the per-frame sort
outweighing the early-z savings) — so it's worth keeping available but enabling
only once you've measured it a net win for your content. Both are free to flip
at runtime, so the honest answer is always "profile your own scenes."

**Back-face culling is intentionally not an engine pass.** The engine only sees
anonymous projected triangles; a game's objects know their face normals and
cull back faces at emit time (e.g. `render.c`'s `emit_cube`), which is cheaper
and safe regardless of winding. Keep it game-side.

**Buffer caps & placement.** The triangle and edge lists are fixed buffers
(`SCENE_TRI_CAP` / `SCENE_LINE_CAP`, 4096 each); submitting past a cap silently
drops the extra geometry. A few thousand triangles is well within budget. They
are allocated in **internal SRAM** (with a PSRAM fallback if internal RAM is
too tight) — `scene_init()` logs which it got. Internal placement is what makes
the `scene_prepare()` overlap pay off: the emit/cull/order passes work the
lists off the PSRAM bus, so they run in true parallel with a concurrent PSRAM
framebuffer blit instead of contending for it.

## Depth buffer

Depth is a scaled reciprocal-z (1/z), the quantity that interpolates linearly
in screen space — so the per-pixel inner loop is one add, no divide. Larger
encoded value = nearer.

The depth buffer is **never bulk-cleared**. A per-pixel "frame stamp" records
which frame last wrote each depth; a depth counts only if its stamp is the
current frame, so a stale pixel reads as infinitely far. That makes
`scene_begin()` a single counter increment instead of a full-screen memset, and
confines depth traffic to the pixels the scene actually touches.

Depth and stamp share **one `uint32` cell per pixel** (`stamp << 16 | depth`),
not two separate planes. The rasterizer is PSRAM-latency-bound, and folding
them halves the distinct cache lines the per-pixel depth test touches — one
combined array plus the framebuffer, instead of a depth plane, a stamp plane
and the framebuffer. The stamp is the high 16 bits, so it wraps every 65536
frames (the one-frame, one-pixel mis-resolve that could in principle cause is
invisible in practice); frame 0 is skipped on wrap so a zero-initialised cell
never matches a live frame.

## What the engine does and doesn't do

- **Does:** projection through a 6-DOF camera, the near-clip guard, per-pixel
  depth test + write, flat-shaded triangle fill, depth-biased wireframe, opt-in
  frustum cull + front-to-back ordering.
- **Doesn't (yet / by design):** lighting, texturing, per-vertex colour,
  back-face culling (game-side, by design). **The game owns shading** — compute
  a face colour (e.g. per-face lighting from a normal) and pass it as the
  triangle's `argb`. **The game owns its object/world model** — the engine
  never sees "objects", only triangles and edges (see [objects.md](objects.md)).

## Tuning notes

- It's fill-bound: cost scales with on-screen pixels, not triangle count per
  se. Low-poly models, back-face culling game-side, and the opt-in
  `frustum_cull` / `depth_order` passes are the levers.
- The wireframe overlay hides the occasional 1-px seam from the non-sub-pixel
  triangle fill; a wireframe-free look would want a half-space rasteriser
  (not currently provided).
