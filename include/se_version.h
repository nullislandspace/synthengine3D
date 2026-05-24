#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API
// ---------------------------------------------------------------------
//  This header is part of the engine's public, semver'd surface.
//  Everything under synthengine3D/include/ is the stable contract:
//  breaking changes here bump SE_VERSION_MAJOR. Anything under
//  synthengine3D/src/ (including src/internal/) is INTERNAL and may
//  change or disappear at any patch release -- never include it from
//  game code.
//
//  Versioning contract:
//    * MAJOR -- incompatible public-API change (signatures, semantics,
//               removed symbols, changed public struct layout).
//    * MINOR -- backwards-compatible additions to the public API.
//    * PATCH -- internal-only changes (optimisation, refactor, bugfix)
//               with no public-API effect.
// =====================================================================

#define SE_VERSION_MAJOR 0
#define SE_VERSION_MINOR 2
#define SE_VERSION_PATCH 0

// Returns the engine version as a static "MAJOR.MINOR.PATCH" string.
// Never NULL; the storage is static and outlives the call.
char const* se_version_string(void);
