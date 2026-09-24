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
scene_tri(..., 0);  scene_line(...);   // submit world-space geometry, any order
scene_render(SE_RENDER_ZBUFFER);    // rasterize the whole frame, then reset
```

- **`scene_tri(x0..z2, argb, flags)`** — a filled, flat-shaded, depth-tested
  triangle. `flags` is a mask of `SE_TRI_*` bits; 0 is a plain triangle. The one
  defined so far is `SE_TRI_EMISSIVE`: the triangle is never lit and keeps its
  colour at full strength, for flames, lamps, screens, or geometry the game has
  shaded itself. Undefined bits are reserved and must be 0. `scene_textured_tri`
  takes the same flags.
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

`scene_tri` / `scene_line` / `scene_textured_tri` / `scene_point` do **not**
draw on the call. They project with the current camera and **accumulate** into
per-frame lists;
`scene_render()` does all the rasterization at once. Holding the whole frame is
what lets the engine own the algorithm and cull / order the geometry centrally
without any game call site changing:

```
submit ─▶ [tri list] [edge list] ─▶ scene_render: cull ─▶ order ─▶ rasterize
                                                  (opt-in)(opt-in) (z-buffer)
```

`se_render_mode_t` selects the algorithm: `SE_RENDER_ZBUFFER`
(= `SE_RENDER_DEFAULT`), or one a game registers.

**Near-plane clipping** happens at submit time, in camera space, against
`RENDER_NEAR_CLIP_Z` (it is not the central cull):

- A triangle wholly in front goes into the list as is.
- A triangle wholly behind is dropped.
- A triangle that crosses the plane is **clipped** to it and becomes one or
  two triangles. Texture coordinates are interpolated along the cut edges and
  the lighting of the original face is kept.
- An edge that crosses the plane has its far-side endpoint moved onto the
  plane. A point behind it is dropped.

So a camera can fly close past (or through) geometry without triangles
distorting. Before 2.0, a behind-plane vertex was clamped onto the plane
instead, which bent a straddling triangle out of shape.

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
drops the extra geometry. The textured-triangle and point lists are allocated on
first use (`SE_SCENE_TEXTURED_TRI_CAP` 1024, `SE_SCENE_POINT_CAP` 1024; the
point list always in PSRAM) and overflow the same way. A clipped triangle can
take two entries. A few thousand triangles is well within budget. They
are allocated in **internal SRAM** (with a PSRAM fallback if internal RAM is
too tight) — `scene_init()` logs which it got. Internal placement is what makes
the `scene_prepare()` overlap pay off: the emit/cull/order passes work the
lists off the PSRAM bus, so they run in true parallel with a concurrent PSRAM
framebuffer blit instead of contending for it.

## Viewport (`scene_set_viewport`)

```c
scene_set_viewport(&(se_viewport_t){ .x = 0, .y = 40, .w = 800, .h = 400 });
se_viewport_t vp = scene_viewport();
scene_set_viewport(NULL);   // back to the whole screen
```

Every rasterizer touches only pixels inside the rectangle (logical pixels;
`w` / `h` are sizes), and `frustum_cull` culls against it, so a smaller
viewport also culls more. It is persistent — not reset by `scene_begin()` —
and it clips, it does not re-frame: the projection is still the `RENDER_*`
pinhole, so an off-centre viewport shows an off-centre crop. Move the vanishing
point with the `RENDER_*` overrides if the window needs it. It stays in
full-screen pixels at quarter resolution.

## Quarter resolution (`scene_set_render_scale`)

For a scene that is fill-bound (a screen full of textured surfaces), the engine
can render every other pixel of every other line — a quarter of the pixels —
into a buffer half the size each way, which the game then scales up:

```c
// Once: a half-size buffer in the display's format and orientation.
se_display_info_t di;
se_display_info(&di);
static se_ppa_layer_t half;
se_ppa_layer_alloc(&half, DISPLAY_LOG_W / 2, DISPLAY_LOG_H / 2, di.pax_format, di.reversed, di.orientation);

// Per frame (on_backdrop / on_render):
scene_set_render_scale(2);          // latched by scene_begin()
/* backdrop into half.buf */
scene_begin(&half.buf);
/* submit exactly as at full resolution */
scene_prepare(SE_RENDER_ZBUFFER);
scene_rasterize(SE_RENDER_ZBUFFER);
se_ppa_layer_sync(&half);           // the CPU's pixels to PSRAM, out of the cache
se_ppa_blit_scaled(fb, 0, &half, 2);
se_ppa_wait_job(0);
se_ppa_buf_invalidate(fb);          // only if the CPU reads fb afterwards (a screenshot)
```

- **Nothing else changes.** The camera, the `RENDER_*` projection, the
  viewport (still in full-screen pixels), culling, near clipping and lighting
  all work in full-screen coordinates. Only the projected positions are halved,
  at the end of the projection, so pixel `(i, j)` of the half-size target is
  exactly what full resolution draws at `(2i, 2j)` (up to float rounding).
- **Per frame.** The scale is latched by `scene_begin()`, so a game can switch
  between scenes or shots freely (`scene_render_scale()` reads back the
  requested one); at scale 1 the output is bit for bit what it always was.
- Lines and points stay one target pixel wide: two screen pixels.
- A custom renderer sees `se_geometry_t.scale` and must address the
  half-size target itself.
- **The upscale.** `se_ppa_blit_scaled()` runs on the PPA and costs the CPU
  nothing, but the PPA's scaler **interpolates** (it has no nearest-neighbour
  setting), so the pixels come out soft rather than as crisp 2×2 blocks. The
  cache syncs are needed because the half-size buffer is small enough to stay
  in the cache between frames (see [ppa.md](ppa.md#cache-coherency)).
- **Cost.** In the showreel's block world, rasterizing fell to about a third
  (138 → 40 ms in a textured walk; the per-triangle setup does not shrink);
  the fills, the sync and the upscale add about 6.5 ms of waiting per frame.

## Custom renderers (`se_renderer_register`)

The resolve step is a seam: a game can register its own renderer and pass its
handle to `scene_render()` / `scene_prepare()` / `scene_rasterize()` like a
built-in, without touching a single `scene_tri` call site (cel shading,
dithering, a depth-cued fog pass...).

```c
static void my_prepare(void* user)   { /* geometry only: NO framebuffer pixels */ }
static void my_rasterize(void* user) {
    se_geometry_t const g = se_scene_geometry();   // this frame's lists + targets
    /* draw g.tris / g.segs into g.fb, depth-testing g.depth ... */
    se_scene_raster_textured();                    // the engine's textured pass
    se_scene_raster_points();                      // and its point pass, last
}
se_render_mode_t const MINE = se_renderer_register(&(se_renderer_t){
    .name = "mine", .prepare = my_prepare, .rasterize = my_rasterize });
```

- The engine's cull and order passes run before `prepare()`, so a custom
  renderer gets an already culled (and, if enabled, sorted) list.
- `se_scene_geometry()` is only valid inside the callbacks. Vertices are in
  screen space (`sx`, `sy`, and `w` = 1/z, larger is nearer; multiply by
  `depth_scale` to encode). The depth plane is `(stamp << 16) | depth` per
  pixel and never cleared: a cell counts only if its stamp equals `frame`.
  At quarter resolution (`scale` 2) the target and the screen positions are
  half size.
- `se_renderer_name()` names a handle for logs (`"?"` if unknown). A full
  table or a malformed renderer returns `SE_RENDER_DEFAULT`, so a caller that
  ignores the result still renders.

## Statistics

For profiling, each of these reports the most recent `scene_rasterize()` (any
pointer may be NULL):

- `scene_raster_stats(&tri_n, &line_n, &tri_us, &line_us)` — flat triangles and
  edges, post-cull.
- `scene_textured_stats(&ttri_n, &ttri_us)` — the textured pass.
- `scene_point_stats(&pt_n, &pt_us)` — the point pass.
- `se_present_stats(&blit_us, &vsync_us)` ([`se_run.h`](../include/se_run.h)) —
  the present after the frame: the page flip, and the wait for the display
  to pick up the previous frame (zero unless the game is faster than the
  refresh). Read from `on_render`, it is the previous frame's.

## Depth buffer

Depth is a scaled reciprocal-z (1/z), the quantity that interpolates linearly
in screen space — so the per-pixel inner loop is one add, no divide. Larger
encoded value = nearer. It is stored as 16 bits, `64000 × RENDER_NEAR_CLIP_Z / z`,
so the nearest drawable point always uses the full range: one depth step is
about z² / (64000 × near), and nothing beyond z = 64000 × near is drawn (at
the default near plane of 0.5, one step is 0.003 at z = 10 and 0.31 at
z = 100; the far limit is 32000).

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

- **Does:** projection through a 6-DOF camera, near-plane clipping, per-pixel
  depth test + write, flat-shaded triangle fill, perspective-correct textured
  triangles ([`se_texture.h`](../include/se_texture.h), see below),
  depth-biased wireframe, depth-tested points (`scene_point`: 1 px, unlit,
  drawn last, never written to depth -- e.g. a starfield), opt-in frustum cull + front-to-back ordering, opt-in
  single-light shading ([`se_light.h`](../include/se_light.h), see below).
- **Also:** a clipping viewport, quarter-resolution rendering and pluggable
  renderers (sections below).
- **Doesn't (yet / by design):** texture filtering or mipmaps, blended (partial) transparency,
  per-vertex colour, more than one light, shadows, distance falloff, specular,
  back-face culling (game-side, by design). **The game owns its object/world model** — the engine never sees
  "objects", only triangles and edges (see [objects.md](objects.md)).

## Lighting (`se_light.h`)

Optional, and off unless a game calls `se_light_set()`. One positional light:

```c
se_light_set(&(se_light_t){ .x = -4.0f, .y = 2.2f, .z = -1.0f,
                            .brightness = 0.45f, .two_sided = true });
```

`brightness` is the **directional share of the total illumination**, 0..1; the
remainder is global illumination that reaches every surface. With `d` = how
squarely the face meets the light:

    shade = (1 - brightness) + brightness * d

So a face square-on to the light keeps its full colour, and a face turned away
falls to `1 - brightness` — never to black unless `brightness` is 1.0.

Applied **per triangle in `scene_tri` (and `scene_textured_tri`), at submit
time** — not per pixel. A triangle is one flat colour on screen, so per-pixel
shading would buy nothing, and the textured pass modulates the same per-face
value. The light can change every frame (a sunset); `se_light_get()` reads it
back, and `se_light_set(NULL)` turns lighting off.
The engine derives the normal from the world-space vertices it is handed, which
is why `scene_tri` takes world space rather than pre-projected coordinates.

Because the engine does not mandate a winding, `two_sided` orients each normal
towards the camera so the visible side is the lit side. Set it `false` to use
the raw cross-product normal, which is correct only for CCW-outward winding.

Wireframe edges (`scene_line`) are never lit — a line has no normal. Note the
game still owns **back-face culling**, so a lit game computes the face normal
once for its own cull and the engine computes it again for the shade; at a few
hundred triangles a frame that duplication is far cheaper than an API that
makes the game hand its normals over.

## Textured triangles (`se_texture.h`, `scene_textured_tri`)

A third primitive next to `scene_tri` and `scene_line`, which are unchanged:

```c
se_texture_t* metal = se_texture_load("/sd/apps/my.app/metal.png", SE_TEXTURE_INTERNAL);

se_tex_vertex_t const v[3] = {
    {x0, y0, z0, 0.0f, 0.0f},   // world position, then u, v
    {x1, y1, z1, 1.0f, 0.0f},
    {x2, y2, z2, 0.0f, 1.0f},
};
scene_textured_tri(v, metal, 0);
```

- **Textures** are PNGs, decoded by libspng (which graceloader carries) into
  RGB565. Both edges must be a power of two, up to `SE_TEXTURE_MAX_DIM`.
  `SE_TEXTURE_INTERNAL` puts the texels in internal SRAM, falling back to PSRAM
  (logged, and reported in `tex->internal`) if it won't fit. Load between
  frames (it reads a file); `se_texture_unload()` frees one, but not while a
  triangle submitted this frame still uses it (the texels are read at
  rasterize time).
- **Cut-out transparency.** Alpha is one bit: a texel with PNG alpha below 128
  is a hole, stored as `SE_TEXEL_CUTOUT`, and draws neither colour nor depth,
  so whatever is behind it shows through. For leaves, fences, grass sprites and
  window frames. No sorting is needed: the depth test handles the rest. There
  is no blending. Only textures with a hole (`tex->cutout`) take the cut-out
  loop, which fetches the texel before writing; opaque textures run the
  unchanged loop, so their output is bit for bit what it was. `mean_argb`
  averages the opaque texels only.
- **Coordinates:** `(0,0)` is the texture's top-left, `(1,1)` its bottom-right,
  and anything outside repeats. Mapping is perspective-correct: `u·w` and `v·w`
  interpolate linearly and are divided by `w` per drawn pixel. Sampling is
  nearest-texel.
- **Its own list.** Textured triangles are deferred into a separate list, which
  isn't allocated until the first texture loads. They rasterize after the flat
  triangles and before the edges, depth-tested against both, so the three
  primitives mix freely. Cull and order apply to that list as they do to the
  flat one. A custom renderer sees them in
  `se_geometry_t.ttris` and can call `se_scene_raster_textured()`.
- **Lighting** applies as it does to flat triangles: the same per-face shade,
  computed at submit time, kept as a 0..32 factor and applied to each texel.
  `SE_TRI_EMISSIVE` skips it, and the texels are drawn as loaded.
- **Cost.** The divide and the texel fetch only happen for pixels that pass the
  depth test, but that is still far more work per pixel than a flat fill.
  `scene_textured_stats()` reports the pass separately from
  `scene_raster_stats()`. See the showreel's `devdocs/performance.md` for
  measured numbers.

## Tuning notes

- It's fill-bound: cost scales with on-screen pixels, not triangle count per
  se. Low-poly models, back-face culling game-side, and the opt-in
  `frustum_cull` / `depth_order` passes are the levers — and, for a scene that
  fills the screen with textured surfaces, quarter resolution, which cuts the
  rasterizing to roughly a third (per-triangle setup does not shrink).
- Textured pixels cost several times flat ones (a divide and a texel fetch per
  drawn pixel). Cut-out textures cost the same per pixel, but whatever shows
  through their holes is drawn too.
- The wireframe overlay hides the occasional 1-px seam from the non-sub-pixel
  triangle fill; a wireframe-free look would want a half-space rasteriser
  (not currently provided).
