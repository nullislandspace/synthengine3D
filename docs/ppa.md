# PPA compositor (`se_ppa.h`)

A thin, generic wrapper over the **ESP32-P4 PPA** (Pixel-Processing
Accelerator) for offloading 2D blit work — backdrops, sprite layers, full-band
fills — off the CPU. It owns the parts every app re-writes when it reaches for
the PPA: the client lifecycle, an asynchronous completion latch, the
logical→raw orientation maths, and cache-line-aligned PSRAM layer caches.

> **ESP32-P4 only.** The PPA is a P4 peripheral; on other targets this
> subsystem isn't available. Orientation support is `PAX_O_UPRIGHT` and
> `PAX_O_ROT_CW` (the Tanmatsu panel), verified; other orientations are refused
> (logged) rather than mis-blitted, until a port adds and tests their maths.

## The model

Three operations, all on **full-width logical row bands** `[y_top, y_top + h)`:

| Call | PPA op | Use |
|---|---|---|
| `se_ppa_fill(fb, y_top, h, argb)` | FILL | paint a band one colour (e.g. a sky) |
| `se_ppa_blit(fb, layer, dst_y_top)` | SRM (1:1) | copy a layer cache into a band |
| `se_ppa_blend_key(fb, layer, dst_y_top, ck_lo, ck_hi)` | BLEND | composite a layer with a foreground colour-key |

Geometry and orientation are read off the `pax_buf_t` arguments — you think in
**logical** (post-orientation) screen coordinates; the engine converts each
band to the raw PPA picture-block.

**What stays yours:** the artwork in the layers, the band layout (which rows,
which colour, which key), and the *choreography* — the order of submits and
where the waits sit (see [ordering](#ordering--async)). That's application
policy, not generic PPA work.

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

Submits are **non-blocking**: they kick the hardware and return. Completion is a
**counting-semaphore latch** — `se_ppa_wait_one()` blocks for one op,
`se_ppa_wait_all()` drains the batch, `se_ppa_pending()` reaps completed ops
without blocking and returns how many are still outstanding.

The key fact: **PPA does not guarantee execution order across different client
types** (FILL vs SRM vs BLEND). So:

- **Overlapping ops must be serialised by you.** A backdrop where sky/sun/hills
  all write the same upper region submits `fill → wait_one → blit → wait_one →
  blend → wait_one`. Without the barriers the sun could land on top of the
  hills.
- **Independent regions can batch.** Several non-overlapping sprite blits can be
  fired in a burst and drained with one `se_ppa_wait_all()` — more PPA/CPU
  parallelism. The latch correctly handles ops that finished *before* you wait
  (the counting semaphore accumulated their completions).

You can also overlap CPU work with the hardware: submit, do unrelated CPU
drawing, then `se_ppa_wait_all()`. Race the Synth's backdrop does exactly this —
it submits the mountain BLEND, paints the CPU floor grid while the BLEND runs,
then waits.

## Error handling

The `bool` returns are the error channel, and **the completion latch can never
desync**:

- A submit returns `false` and queues **nothing** if the in-flight cap
  (`SE_PPA_MAX_PENDING`) is reached, the driver rejected the op (e.g. a client's
  queue is full), or the band is unsupported/out of bounds. The pending count
  only advances on an *accepted* op.
- Recover by draining and retrying:
  ```c
  if (!se_ppa_blit(fb, &spr, y)) { se_ppa_wait_one(); se_ppa_blit(fb, &spr, y); }
  ```
- `se_ppa_wait_one()` has a 50 ms timeout; on timeout it logs and does **not**
  decrement (a late completion could still signal). PPA ops are sub-millisecond,
  so a timeout means something is wedged — it then surfaces as submits returning
  `false`, visibly, instead of as a corrupted latch.

A failed `se_ppa_init()` (or layer alloc) is non-fatal: every later submit is a
no-op returning `false`, so a PPA problem degrades to "no backdrop", never a
crash.

## Cache coherency

Simpler than it looks, because of *who writes what*:

- **Layer caches** are CPU-drawn once → one **cache-to-memory flush**
  (`se_ppa_layer_flush`) pushes the pixels to PSRAM before the PPA's DMA reads
  them. They never change after, so that's the only cache op in the pipeline.
- **The framebuffer** needs no per-frame flush or invalidate in the common
  backdrop case: during the backdrop phase its target region is written *only*
  by the PPA (FILL/BLIT/BLEND are all DMA), and any CPU drawing (a floor grid,
  the 3D scene) goes to *other* regions. PPA-vs-PPA ordering is handled by the
  waits, not cache ops (DMA peers are coherent with PSRAM). If a future app
  reads PPA output back with the CPU it would need an invalidate — not provided
  yet, by design (the proven path doesn't use one).

## Threading

**Single producer.** All submits and waits must come from one task (your render
task); the completion ISR only signals the semaphore. The in-flight counter is
mutated only in task context, so there's no lock.

## Tunables

Overridable `#define`s in [`se_config.h`](configuration.md#ppa-compositor-se_ppa):

- `SE_PPA_MAX_PENDING` (8) — in-flight cap. Sizes *both* the completion
  semaphore and the submit guard from one value, so they can't drift. Raise it
  for an app batching many async blits.
- `SE_PPA_CLIENT_QUEUE_DEPTH` (1) — per-client queue depth (`max_pending_trans_num`).
- `SE_PPA_CACHE_LINE` (128) — PSRAM L2 cache-line size for aligned alloc + flush.

## See also

- [`../examples/backdrop/`](../examples/backdrop/) — a complete sky/sun/hills
  backdrop on `se_ppa`.
- [integration.md](integration.md) — the two extra IDF components the PPA pulls
  in (`esp_driver_ppa`, `esp_mm`).
- [renderer.md](renderer.md) — the *3D* scene pipeline (`se_scene`), a separate,
  CPU-software path; the PPA helper is for 2D backdrop/sprite offload.
