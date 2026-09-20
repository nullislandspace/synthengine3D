#pragma once
// =====================================================================
//  SynthEngine3D  --  host stand-in for the umbrella header
// ---------------------------------------------------------------------
//  Game code says #include "synthengine3d.h". On a host build this file
//  answers that instead of include/synthengine3d.h, so put host/shims on
//  the include path BEFORE include/ (see docs/testing.md).
//
//  It pulls in the REAL headers for everything the harness implements,
//  so every type a checker sees is the engine's own; what it leaves out
//  is the run loop, audio, UI, save and the rest, which have no meaning
//  off the badge.
// =====================================================================
#include "se_config.h"
#include "se_light.h"
#include "se_scene.h"
#include "se_texture.h"
