#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  engine splash screen
// ---------------------------------------------------------------------
//  A short 3D title sequence the engine draws for itself: the wordmark
//  as real 3D geometry, flying toward the camera and easing to a stop.
//  Part of the versioned public surface (see se_version.h).
//
//  This is a BLOCKING call with its own frame loop -- it draws and
//  presents its own frames until the animation ends, then returns. Call
//  it from on_init(), after any engine parameters you want set (the
//  projection constants in se_config.h are compile-time, so in practice
//  there is nothing to sequence it against):
//
//      static void on_init(void* user) {
//          se_splash();            // ~1 s, returns when done
//          ...game init...
//      }
//
//  It requires se_run() to have bootstrapped (it borrows the engine's
//  framebuffers and vsync), so calling it from app_main() BEFORE se_run()
//  does nothing but log a warning.
//
//  The text is drawn as Hershey vector strokes emitted through
//  scene_line() -- genuine world-space geometry projected by the engine's
//  own camera, not a 2D bitmap that gets scaled. That is why it looks
//  right at every size, and why the zoom is a real perspective approach
//  rather than a blit stretch.
// =====================================================================

// Show the default splash: "SynthEngine 3D" over the engine's version,
// zooming in for about one second. Returns when the animation completes.
void se_splash(void);

// Same, with control over the wording and duration.
//   title     NULL -> "SynthEngine 3D"
//   subtitle  NULL -> "Version <se_version_string()>", so it tracks the
//                     engine rather than going stale. Pass your own to
//                     override (a game's own name, a build tag, "").
//   seconds   <= 0 -> 1.0
void se_splash_ex(char const* title, char const* subtitle, float seconds);
