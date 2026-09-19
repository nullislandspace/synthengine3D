#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  textures
// ---------------------------------------------------------------------
//  Load a PNG file into a texture the scene pipeline can map onto
//  triangles (scene_textured_tri, se_scene.h), and free it again. Part of
//  the versioned public surface (see se_version.h).
//
//  FORMAT. Texels are stored as RGB565 in the CPU's native byte order,
//  one uint16 each, row-major -- the same colour depth as the
//  framebuffer, so drawing one costs a lookup and not a conversion. The
//  PNG may be any colour type or bit depth libspng can decode; it is
//  converted to RGB565 on load.
//
//  CUT-OUT TRANSPARENCY. Alpha is reduced to one bit: a texel whose PNG
//  alpha is below 128 is a HOLE, stored as the reserved value
//  SE_TEXEL_CUTOUT, and the rasterizer draws neither its colour nor its
//  depth -- what is behind shows through, and the depth test sorts it
//  out, so cut-out triangles need no ordering. Opaque texels are drawn
//  as always. There is no blending (partial alpha). An opaque texel
//  that would convert to SE_TEXEL_CUTOUT itself is stored one blue step
//  darker, so it stays drawn. A texture with at least one hole has
//  `cutout` set; only those take the (slightly different) cut-out
//  raster path, so opaque textures render exactly as before.
//
//  SIZE. Both edges must be a power of two, from 1 up to
//  SE_TEXTURE_MAX_DIM (se_config.h). That is what lets the rasterizer
//  wrap texture coordinates with a mask instead of a modulo in its inner
//  loop, and it is enforced: any other size fails to load, with a log
//  line saying so, rather than being silently resampled.
//
//  WHERE THE TEXELS LIVE. PSRAM by default. SE_TEXTURE_INTERNAL asks for
//  internal SRAM instead. The rasterizer reads a texel for every pixel it
//  draws, and those reads land wherever the triangle happens to map, not
//  in the neat runs the framebuffer gets. So a small, heavily used
//  texture is worth the internal RAM. If internal SRAM cannot hold it,
//  the loader falls back to PSRAM and logs that. The load still
//  succeeds, and `internal` reports where it really went, so a game that
//  cares can check.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

// The texel value that marks a hole (RGB565 magenta).
#define SE_TEXEL_CUTOUT  0xF81Fu

// Load flags, OR-ed together.
#define SE_TEXTURE_INTERNAL  (1u << 0)   // texels in internal SRAM (PSRAM if it will not fit)

typedef struct {
    uint16_t* texels;   // w * h RGB565 texels, native byte order, row-major
    int       w, h;     // size in texels; both powers of two
    uint8_t   w_log2;   // log2(w): row stride as a shift
    bool      internal; // true if the texels ended up in internal SRAM
    uint32_t  mean_argb;// average colour of its opaque texels (ARGB8888)
    bool      cutout;   // true if any texel is a hole (SE_TEXEL_CUTOUT)
} se_texture_t;

// Load the PNG at `path` (a full VFS path, e.g. "/sd/apps/my.app/metal.png")
// and return a new texture, or NULL on failure. Failures are logged:
// missing file, a PNG libspng cannot decode, a size that is not a power
// of two or exceeds SE_TEXTURE_MAX_DIM, or no memory. `flags` is a mask
// of SE_TEXTURE_* values; 0 means PSRAM.
//
// Call it from on_init() or between frames, not during one: it reads a
// file, allocates, and briefly needs 4 bytes per texel of PSRAM scratch
// for the decode. A game that knows its asset directory under
// graceloader gets it from graceloader_get_install_basepath().
se_texture_t* se_texture_load(char const* path, uint32_t flags);

// Free a texture and its texels. NULL is a no-op.
//
// Not safe while any triangle submitted this frame still points at it:
// scene_textured_tri() stores the pointer and the texels are read at
// scene_rasterize(). Unload between frames, or after the frame that
// last used it has rasterized.
void se_texture_unload(se_texture_t* tex);
