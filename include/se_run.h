#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  application framework
// ---------------------------------------------------------------------
//  The engine's run loop (inversion of control): the game calls se_run()
//  once and the engine owns the frame loop, the device bootstrap, the
//  input-queue pump, the device-global keys, vsync/blit, and the backdrop
//  clear. The game plugs in via the callbacks below — it is content +
//  per-frame logic, not loop plumbing. Part of the semver'd public surface
//  (see se_version.h). Overview + the frame lifecycle: docs/architecture.md;
//  quick start: docs/getting-started.md.
// =====================================================================

#include <stdbool.h>
#include <stddef.h>      // size_t (se_display_info_t)
#include <stdint.h>

#include "bsp/input.h"   // bsp_input_event_t (forwarded to on_input)
#include "pax_gfx.h"     // pax_buf_t (the frame buffer handed to draw cbs)

// One-time configuration handed to se_run(). Zero-initialise and set what
// you need; defaults are sensible.
typedef struct {
    // If true, the engine handles F1 itself as "return to launcher"
    // (shutting audio down first). If false, F1 is forwarded to on_input
    // like any other key. NOTE: the engine never touches the power button
    // (2 s-hold power-off lives in the coprocessor) or F2/F3.
    bool     f1_exits;

    // Colour the engine clears the framebuffer to each frame when no
    // on_backdrop callback is registered (ARGB8888).
    uint32_t backdrop_argb;

    // (Room to grow without breaking the ABI: display orientation hint,
    // audio sample rate, target frame cap, … — all defaulted when 0.)
} se_app_config_t;

// Per-frame / lifecycle callbacks. Every pointer is optional except
// on_update; NULL is skipped. `user` is the opaque context passed to
// se_run() and threaded to every callback (the game's state).
typedef struct {
    // Called once after the engine has finished bootstrap (display,
    // audio, input, scene, device-global settings) and before the first
    // frame. Do game-side init here (world, save, content load).
    void (*on_init)(void* user);

    // Called for each queued input event the engine did NOT consume
    // itself (it consumes volume ±, audio-jack, and F1 when f1_exits).
    // Polled steering stays a direct BSP read inside on_update.
    void (*on_input)(bsp_input_event_t const* ev, void* user);

    // Per-frame game logic / state machine. `dt` is seconds since the
    // previous frame (clamped to a sane max). Required.
    void (*on_update)(float dt, void* user);

    // Optional: draw the backdrop into `fb` at the start of the frame.
    // If NULL, the engine clears `fb` to cfg.backdrop_argb instead.
    void (*on_backdrop)(pax_buf_t* fb, void* user);

    // Draw the frame (3D scene via se_scene, HUD, menus) into `fb`,
    // after the backdrop and before the engine blits at vsync.
    void (*on_render)(pax_buf_t* fb, void* user);

    // Optional: called once if the loop is ever asked to stop
    // (se_request_exit). On graceloader, F1 reboots to the launcher and
    // this never runs; provided for completeness / native ports.
    void (*on_shutdown)(void* user);
} se_app_callbacks_t;

// Bootstrap the device + engine, then run the frame loop until
// se_request_exit() (or, on graceloader, until F1 reboots). Does not
// return under normal graceloader operation. `cfg` may be NULL (all
// defaults); `cb` is required (and cb->on_update must be set).
void se_run(se_app_config_t const* cfg, se_app_callbacks_t const* cb, void* user);

// Ask the run loop to exit after the current frame (then se_run returns,
// firing on_shutdown). A no-op before se_run() is entered.
void se_request_exit(void);

// Resolved display geometry + pixel layout the engine derived from the
// BSP at bootstrap. Valid from on_init() onward (the engine has already
// initialised the display and allocated the framebuffers by then). A game
// needs these to build its own auxiliary buffers (e.g. backdrop layer
// caches) and to drive raw-framebuffer hardware blocks (PPA bands) in the
// same orientation/format the engine's framebuffers use.
typedef struct {
    size_t            width;       // raw framebuffer width  (BSP h-res, pixels)
    size_t            height;      // raw framebuffer height (BSP v-res, pixels)
    pax_buf_type_t    pax_format;  // PAX buffer type matching the LCD format
    bool              reversed;    // true if pixel byte order is big-endian
    pax_orientation_t orientation; // PAX orientation applied to the framebuffers
} se_display_info_t;

// Fill *out with the resolved display info (see above). Safe to call any
// time after the engine has bootstrapped (i.e. from on_init onward); a
// no-op if `out` is NULL.
void se_display_info(se_display_info_t* out);
