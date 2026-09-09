#pragma once
// =====================================================================
//  SynthEngine3D  --  INTERNAL  --  frame machinery access
// ---------------------------------------------------------------------
//  se_run.c owns the two framebuffers and the blit/vsync/swap. A few
//  engine facilities are blocking modal sub-loops that must draw and
//  present frames of their own outside the normal on_render path -- the
//  rebind-key prompt (which lives in se_run.c and uses the statics
//  directly) and the splash screen (which does not). This is the seam
//  for the latter.
//
//  Engine-internal: src/internal/ is never on a consumer's include path.
// =====================================================================

#include "pax_gfx.h"

// The current back buffer -- the one safe to draw into. Changes after
// every se_frame_present(), so re-read it each frame rather than caching.
// NULL if se_run() has not bootstrapped yet.
pax_buf_t* se_frame_back(void);

// Blit the back buffer to the LCD, wait for vsync, swap. No-op before
// bootstrap.
void se_frame_present(void);
