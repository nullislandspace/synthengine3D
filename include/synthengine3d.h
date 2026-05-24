#pragma once
// =====================================================================
//  SynthEngine3D  --  umbrella public header
// ---------------------------------------------------------------------
//  Include this single header to pull in the engine's whole public API.
//  It doubles as the table of contents for the stable surface: every
//  public subsystem header is listed below as it lands. Internal headers
//  (synthengine3D/src/internal/) are deliberately NOT reachable from here.
//
//  See README.md for the capability overview and quick start, and
//  docs/ for per-subsystem guides. Stability/versioning policy lives in
//  se_version.h.
// =====================================================================

#include "se_version.h"
#include "se_config.h"          // E0+ -- overridable engine defaults
#include "se_direct565.h"       // E1  -- inline framebuffer primitives
#include "se_text.h"            // E1  -- Hershey vector text
#include "se_audio_source.h"    // E2  -- music_source_t / sfx_voice_t contracts
#include "se_audio_dsp.h"       // E2  -- DSP primitives (osc, env, biquad)
#include "se_audio.h"           // E2  -- software mixer
#include "se_voice.h"           // E2  -- pluggable synth voices (built-in + custom)
#include "se_music_procedural.h"// E2  -- procedural music source
#include "se_nbt.h"             // E3  -- NBT serialization primitive
#include "se_save.h"            // E3  -- file-backed save-slot framework
#include "se_scene.h"           // E4  -- 3D scene renderer + camera + projection
#include "se_ppa.h"             // E7  -- ESP32-P4 PPA blit helper (backdrop offload)
#include "se_run.h"             // EF  -- application framework / run loop (API; impl staged in)
#include "se_hw.h"              // EF  -- device-global hardware settings (volume/brightness)
#include "se_ui.h"              // EF  -- UI / list-menu system
#include "se_bindings.h"        // EF  -- input bindings (remap + persistence)

// Deliberately NOT here:
//   * No se_object.h -- there is no object/world framework (E5 deferred):
//     a game owns its object pool and submits world-space geometry via
//     se_scene's scene_tri / scene_line. See docs/objects.md.
//   * Internal headers (src/internal/) are private and never reachable here.
