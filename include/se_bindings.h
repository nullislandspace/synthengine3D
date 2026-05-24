#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  input bindings
// ---------------------------------------------------------------------
//  Remappable key bindings + their persistence. The game *declares* its
//  controls (a stable id, a label, an NVS key, a default scancode) and an
//  NVS namespace; the engine *owns* loading them (falling back to the
//  declared defaults), persisting changes, and answering the current
//  scancode for a control. The game polls se_bindings_get() for steering
//  etc.; the (forthcoming) engine remap dialog will set them.
//
//  Bindings are raw BSP scancodes: every physical key has one, so a
//  scancode is a complete, channel-independent identifier -- pollable for
//  smooth steering and matchable against key events for edges.
//
//  Part of the semver'd public surface (see se_version.h).
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

// One remappable control, declared by the game.
typedef struct {
    int          id;          // the game's own control id (e.g. its enum)
    char const*  label;       // human label (for the engine remap dialog)
    char const*  nvs_key;     // NVS key (<= 15 chars) this binding persists under
    uint16_t     default_sc;  // default BSP scancode
} se_binding_def_t;

// The game's control set + where to persist it. `defs` is retained by
// reference -- it must outlive use (point it at static storage).
typedef struct {
    char const*             nvs_namespace;
    se_binding_def_t const* defs;
    int                     count;
} se_bindings_config_t;

// Register the control set and load each binding from NVS, falling back
// to its declared default when the key is unset. Call once at boot.
// (`count` is clamped to SE_BINDINGS_MAX -- see se_config.h.)
void se_bindings_init(se_bindings_config_t const* cfg);

// Current scancode bound to control `id` (its declared default if unset;
// 0 if `id` is unknown). Cheap -- safe to poll for steering each frame.
uint16_t se_bindings_get(int id);

// Rebind control `id` to scancode `sc` and persist it to NVS. No-op if
// `sc` is 0, unchanged, or `id` is unknown.
void se_bindings_set(int id, uint16_t sc);
