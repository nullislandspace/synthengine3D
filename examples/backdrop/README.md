# PPA backdrop example

A hardware-composited backdrop on the ESP32-P4 PPA, in the engine's
`on_backdrop` hook. One file, [`main.c`](main.c). **ESP32-P4 only** (the PPA
peripheral).

It composites three layers every frame with zero CPU pixel work:

1. **`se_ppa_fill`** — a flat sky band.
2. **`se_ppa_blit`** — a pre-rendered "sun" layer cache, copied in at a `y`
   that slides up and down (a setting-sun bob).
3. **`se_ppa_blend_key`** — a "hills" layer cache composited with a green
   colour-key, so only the silhouette lands and the sky/sun show through.

## What it shows

- **Setup once** (`on_init`): `se_ppa_init()`, then `se_ppa_layer_alloc()` for
  each layer (matching the framebuffer's format/orientation from
  `se_display_info()`), draw artwork into `layer.buf` with normal PAX calls,
  and `se_ppa_layer_flush()` so the PPA's DMA sees finished pixels.
- **Per frame** (`on_backdrop`): three non-blocking submits, each followed by
  `se_ppa_wait_one()`. The barriers matter — PPA does **not** guarantee
  execution order across different client types, and all three ops write the
  same upper region, so without the waits the sun could land on top of the
  hills. Ops that touch **independent** regions don't need the barriers: fire
  them as a batch and drain once with `se_ppa_wait_all()`.
- **Logical vs raw** (`logical_dims`): the framebuffer is stored raw
  (pre-orientation); a quarter-turn transposes width/height. The helper derives
  logical dimensions from `se_display_info()` so the layers match the screen.

## Notes

- Submits return `bool`: `false` means refused (in-flight cap reached, driver
  busy, or an out-of-range band) — nothing was queued. Drain with
  `se_ppa_wait_one()` and retry. This example stays well under the cap.
- Layer caches are flushed **once** (they never change). The framebuffer needs
  no per-frame cache op here: during the backdrop phase its upper region is
  written only by the PPA (DMA), never the CPU. See
  [`../../docs/ppa.md`](../../docs/ppa.md).

## Building it

Illustrative — not compiled by the host game's build. To run it, make it your
app's source and wire the engine in per
[`../../docs/integration.md`](../../docs/integration.md). The PPA driver pulls
in two extra IDF components (`esp_driver_ppa`, `esp_mm`) — see that doc.

## See also

- [`../../docs/ppa.md`](../../docs/ppa.md) — the full PPA helper contract.
- [`../minimal/`](../minimal/) — the smallest app (run loop + 3D + music).
