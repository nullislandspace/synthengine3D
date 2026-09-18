#pragma once
// =====================================================================
//  SynthEngine3D  --  INTERNAL  --  scene hooks for other engine files
// ---------------------------------------------------------------------
//  INTERNAL: not reachable from a game's include path and not covered
//  by the semver contract.
// =====================================================================

#include <stdbool.h>

// Allocate the textured-triangle list if it does not exist yet. Called
// by se_texture_load() so the list is in place before the first frame
// that needs it, and a game that never loads a texture never pays for it.
// Returns false (logged) if it could not be allocated anywhere.
bool scene_textured_reserve(void);
