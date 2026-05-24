# PPA compositor (`se_ppa.h`)

A thin, generic wrapper over the **ESP32-P4 PPA** (Pixel-Processing
Accelerator) for offloading 2D blit work — backdrops, sprite layers, full-band
fills — off the CPU. It owns the parts every app re-writes when it reaches for
the PPA: the client lifecycle, an ordered job queue driven by a pump task
(submit is a non-blocking enqueue tagged with a job id; the pump runs jobs in
submission order in task context), the logical→raw orientation maths, and
cache-line-aligned PSRAM layer caches.

> **ESP32-P4 only.** The PPA is a P4 peripheral; on other targets this
> subsystem isn't available. Orientation support is `PAX_O_UPRIGHT` and
> `PAX_O_ROT_CW` (the Tanmatsu panel), verified; other orientations are refused
> (logged) rather than mis-blitted, until a port adds and tests their maths.

## The model

Operations on **full-width logical row bands** `[y_top, y_top + h)`, plus a
sprite blit for an arbitrary logical rectangle:

Every submit takes a caller-assigned **`job_id`** as its second argument (used
later by `se_ppa_wait_job`):

| Call | PPA op | Use |
|---|---|---|
| `se_ppa_fill(fb, job_id, y_top, h, argb)` | FILL | paint a band one colour (e.g. a sky) |
| `se_ppa_blit(fb, job_id, layer, dst_y_top)` | SRM (1:1) | copy a full-width layer cache into a band |
| `se_ppa_blit_rect(fb, job_id, layer, sx, sy, w, h, dx, dy)` | SRM (1:1) | copy a `w×h` sub-rect of a layer to `(dx,dy)` |
| `se_ppa_blend_key(fb, job_id, layer, dst_y_top, ck_lo, ck_hi)` | BLEND | composite a layer with a foreground colour-key |

Geometry and orientation are read off the `pax_buf_t` arguments — you think in
**logical** (post-orientation) screen coordinates; the engine converts each
band (or rect) to the raw PPA picture-block. `se_ppa_blit_rect` is the sprite
form: it lets a layer be sized to its artwork's bounding box instead of the
full screen width (much less SRM read/write and a much smaller cache), and lets
you clip a sprite to the screen or a horizon by trimming `w`/`h` (a
non-positive `w`/`h` is a no-op success).

**What stays yours:** the artwork in the layers, the band layout (which rows,
which colour, which key), and *which CPU work overlaps the hardware*. You do
**not** manage submit ordering — the engine runs jobs in submission order (see
[ordering](#ordering--async)).

## Layer caches

A layer is a pre-rendered image in PSRAM you blit/blend from. Allocate it once,
draw into its `pax_buf_t` with the normal PAX calls, and flush it:

```c
static se_ppa_layer_t sun = {0};

se_display_info_t di;  se_display_info(&di);          // match the framebuffer
se_ppa_layer_alloc(&sun, logical_w, 120,
                   di.pax_format, di.reversed, di.orientation);
pax_background(&sun.buf, 0xFFFF8030u);                 // draw artwork…
pax_draw_rect(&sun.buf, 0xFFFF40A0u, 0, 0, logical_w, 40);
se_ppa_layer_flush(&sun);                              // …then flush once
```

`se_ppa_layer_alloc` sizes the raw backing for the orientation, aligns it to
`SE_PPA_CACHE_LINE`, and `pax_buf_init`s `layer.buf` over it. Caches are static
after the flush, so `se_ppa_layer_flush` is a one-shot, not a per-frame cost.
`se_ppa_layer_free` reclaims one (most apps never bother — layers live for the
session).

## Ordering & async

Submits are **non-blocking enqueues**: they build + validate the op, push it on
the pump's queue, and return. A dedicated **pump task** drains the queue and
submits to the hardware **one op at a time, in submission order**, posting each
finished `job_id` to a completion queue. `se_ppa_wait_job(id)` blocks, draining
that queue, until `id` is reached; `se_ppa_wait_all()` drains everything;
`se_ppa_pending()` returns how many submitted jobs aren't yet drained.

Why a pump? **PPA does not guarantee execution order across client types** (FILL
vs SRM vs BLEND), and the obvious fix — submit op N+1 from op N's completion
*callback* — is unsafe, because that callback runs in **ISR context** and the
driver's submit functions take blocking locks (illegal from an ISR). The pump
runs submission in **task context** and serialises ops itself, so:

- **You never manage ordering.** Enqueue sky → sun → hills with ids `0,1,2`; the
  pump runs them in that order, so the overlapping composite is correct without
  any caller barriers. (Execution being in-order also means `wait_job(id)`
  returns only once everything submitted *before* `id` is done too.)
- **You still choose what overlaps.** Enqueue the batch, do unrelated CPU work,
  *then* `se_ppa_wait_job(last)`. Race the Synth enqueues the sky/floor fills,
  runs the 3D-geometry prepare on the CPU while the pump churns them, then waits.

IDs are yours to scheme; restarting at `0` each frame is fine as long as the
frame drains its jobs (waiting for the last one does that).

## Error handling

The `bool` returns are the error channel:

- A submit returns `false` and queues **nothing** if the submit queue is full
  (`SE_PPA_QUEUE_DEPTH` un-drained jobs), the band/rect is unsupported or out of
  bounds, or PPA isn't initialised. **Don't `se_ppa_wait_job()` an id whose
  submit returned `false`** — it never ran, so it never completes (the wait
  would burn its 50 ms timeout and log).
- `se_ppa_wait_job()` / `se_ppa_wait_all()` have a 50 ms per-op timeout; on
  timeout they log and return. PPA ops are sub-millisecond, so a timeout means
  something is wedged. The pump records a completion even for an op the driver
  rejected, so a waiter never hangs on a job that failed *after* enqueue.

A failed `se_ppa_init()` (or layer alloc) is non-fatal: every later submit is a
no-op returning `false`, so a PPA problem degrades to "no backdrop", never a
crash.

## Cache coherency

Simpler than it looks, because of *who writes what*:

- **Layer caches** are CPU-drawn once → one **cache-to-memory flush**
  (`se_ppa_layer_flush`) pushes the pixels to PSRAM before the PPA's DMA reads
  them. They never change after, so that's the only cache op in the pipeline.
- **The framebuffer** needs no per-frame flush or invalidate. PPA-vs-PPA
  ordering is handled by the waits, not cache ops (DMA peers are coherent with
  PSRAM). The subtle case is **PPA writes a region, then the CPU draws over
  it** — the sky backdrop with the 3D scene on top, or (in Race the Synth) the
  PPA floor base with CPU grid lines + shadow quads on top. That's safe without
  an explicit invalidate for two reasons: (1) the CPU *overwrites* PPA output,
  it never reads it back and uses it, and (2) on a write the CPU read-allocates
  the cache line, and it always gets the fresh PPA pixels because this back
  buffer's prior-frame cache lines were already evicted — the per-frame working
  set (the framebuffer alone is 768 KB, plus the depth/stamp plane) far exceeds
  the L2 cache, so a full alternate-buffer frame flushes and evicts everything
  between two uses of the same buffer. An app whose per-frame working set fits
  in L2, or that reads PPA output back to *use* it, would need an explicit
  invalidate — not provided yet, by design (the proven path doesn't use one).

## Threading

**Single producer.** All submits and waits must come from one task (your render
task). The pump task and the completion ISR are internal: the ISR only signals
the pump, and the pump is the only thing that calls the driver's submit
functions (always in task context). The in-flight counter is touched only by
the producer task, so there's no lock.

## Tunables

Overridable `#define`s in [`se_config.h`](configuration.md#ppa-compositor-se_ppa):

- `SE_PPA_QUEUE_DEPTH` (16) — pump submit/done queue depth = the most jobs that
  may be submitted un-drained at once. A submit past it returns `false`. Raise
  it for an app batching many jobs per frame before waiting.
- `SE_PPA_CLIENT_QUEUE_DEPTH` (1) — per-client driver depth
  (`max_pending_trans_num`). The pump runs one op at a time, so 1 suffices.
- `SE_PPA_PUMP_TASK_PRIO` / `_STACK` / `_CORE` — the pump task's priority,
  stack (words) and core. Default: just under the audio mixer, off the render
  core, so a submit never starves audio and runs concurrently with rendering.
- `SE_PPA_CACHE_LINE` (128) — PSRAM L2 cache-line size for aligned alloc + flush.

## See also

- [`../examples/backdrop/`](../examples/backdrop/) — a complete sky/sun/hills
  backdrop on `se_ppa`.
- [integration.md](integration.md) — the two extra IDF components the PPA pulls
  in (`esp_driver_ppa`, `esp_mm`).
- [renderer.md](renderer.md) — the *3D* scene pipeline (`se_scene`), a separate,
  CPU-software path; the PPA helper is for 2D backdrop/sprite offload.
