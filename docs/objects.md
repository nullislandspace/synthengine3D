# 3D objects: the geometry-submission contract

The engine has **no object/world framework** — that's deliberate (the game's
object pool is too entangled with game-specific concerns like collision,
checkpoints and spawn logic to generalise usefully). The engine's job starts at
the triangle: a game iterates *its own* objects and submits naive world-space
geometry; the engine projects, depth-tests and rasterizes it. This doc is that
contract — everything you need to turn a model into something the renderer
draws correctly.

> Tooling note: a *modelling* pipeline (e.g. OpenSCAD → a C header of verts /
> tris / edges) is game-side, not part of the engine. Race the Synth's version
> lives at `../../devdocs/importing-objects.md`; this doc covers only the
> engine-facing submission rules it builds on.

## Coordinate system + winding

- World space is **x = lateral, y = up, z = forward** (into the screen); the
  camera sits at z = 0 (see [renderer.md](renderer.md)).
- Submit triangles **CCW-outward** (counter-clockwise when viewed from
  outside) and keep winding consistent across a model — that's what lets you
  back-face cull with a normal test. If you import from a Z-up tool, swap axes
  to the engine's Y-up and reverse winding to keep faces CCW-outward.

## The emit pattern

A game object's "emit" transforms its model vertices to world space, then
submits faces as `scene_tri` and feature edges as `scene_line`:

```c
// 1. model -> world (place + scale the instance)
for (each vertex v)
    wx = inst.x + v.x*s;  wy = inst.y_base + v.y*s;  wz = inst.z + v.z*s;

// 2. faces
render_camera_t const cam = render_camera();
for (each tri t) {
    // back-face cull: CCW-outward normal n; skip if it faces away
    //   dot(n, camPos - faceCentre) <= 0  ->  skip   (camPos = {cam.x, cam.y, 0})
    // shade: pick a colour for the face (see below)
    scene_tri(/* 3 world verts */, col_argb);
}

// 3. feature edges (wireframe overlay)
for (each edge e)
    scene_line(/* 2 world verts */, outline_argb);
```

Submit **world-space geometry only — never project or rasterize yourself**. The
z-buffer resolves visibility, so you don't sort, and you can submit objects in
any order.

## Shading is the game's job

The renderer fills a triangle flat with the `argb` you pass — there's no
lighting in the engine. Compute the colour yourself:

- **Back-face culling** (above) — drop faces pointing away from the camera.
  *Optional* but worth it on a fill-bound device. (The engine's future central
  cull will make this redundant; for now it's a game-side win.)
- **Per-face lighting** — a cheap directional model reads well:
  `tint = 0.55 + 0.45 * max(0, dot(n̂, L))` with a fixed light `L`, applied as a
  channel-scale of the base fill. Draw emissive/"light" parts **flat** (no
  tint) instead.
- **Time-based effects** (a pulsing beacon) read the clock in the emit and vary
  the colour — keep them out of the model data.

## Text on 3D surfaces — use lines, not extruded glyphs

Do **not** model text as extruded geometry: a string becomes hundreds–thousands
of triangles. Instead draw it at runtime as **Hershey vector strokes**, which
are just `scene_line` segments — a whole word is ~100–150 cheap lines, and the
string becomes a runtime parameter (data-driven, recolourable). The engine's
Hershey glyph table is exposed for exactly this:

```c
extern int simplex[95][112];   // from se_text.h: glyph index = ASCII - 32
```

Each glyph is a list of vertex pairs (with pen-up markers); map its coords into
your surface's plane and emit `scene_line` per stroke. Hershey Y is already
up, so glyph coords map straight into world-up with no flip. (See `se_text.h`
for the exact glyph format.)

## Keep it cheap

The frame is fill-bound, so per instance the cost is *(visible triangles)* fills
+ *(kept edges)* lines. Keep models low-poly, cull invisible faces, and trim
which edges get an outline. Watch the FPS counter when many instances are on
screen; the `scene_tri`/`scene_line` buffers cap at 4096 each and silently drop
the overflow.
