#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API
// ---------------------------------------------------------------------
//  This header is part of the engine's public, versioned surface.
//  Everything under synthengine3D/include/ is the stable contract:
//  breaking changes here bump SE_VERSION_MAJOR. Anything under
//  synthengine3D/src/ (including src/internal/) is INTERNAL and may
//  change or disappear in any release -- never include it from game
//  code.
//
//  Versioning contract (two parts, MAJOR.MINOR):
//    * MAJOR -- a game has to change to keep building or behaving the
//               same: changed signatures or semantics, removed symbols,
//               changed public struct layout.
//    * MINOR -- everything else: backwards-compatible additions, and
//               internal changes (optimisation, refactor, bugfix) with
//               no public-API effect.
//  There is no third, patch-level number. Games pin the engine by
//  submodule commit, so the commit log already says what a bugfix
//  release would; a separate number for it bought nothing.
// =====================================================================

#define SE_VERSION_MAJOR 2
#define SE_VERSION_MINOR 4

// Returns the engine version as a static "MAJOR.MINOR" string.
// Never NULL; the storage is static and outlives the call.
char const* se_version_string(void);
