// =====================================================================
//  SynthEngine3D  --  textures (se_texture.h)
// ---------------------------------------------------------------------
//  PNG -> RGB565 via libspng, which graceloader already carries (through
//  pax-codecs). The contract -- power-of-two sizes, alpha discarded,
//  internal SRAM on request with a PSRAM fallback -- is in the header.
// =====================================================================

#include "se_texture.h"
#include <stdio.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "se_config.h"
#include "se_scene_internal.h"
#include "spng.h"

static char const* TAG = "texture";

static bool is_pow2(uint32_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

static uint8_t log2_pow2(uint32_t v) {
    uint8_t n = 0;
    while (v > 1) {
        v >>= 1;
        n++;
    }
    return n;
}

// Decode `f` to RGBA8 into a scratch buffer. Returns the buffer (caller
// frees) and the size, or NULL. The file is not closed here.
static uint8_t* decode_rgba8(FILE* f, char const* path, uint32_t* out_w, uint32_t* out_h) {
    spng_ctx* ctx = spng_ctx_new(0);
    if (ctx == NULL) {
        ESP_LOGE(TAG, "%s: spng_ctx_new failed", path);
        return NULL;
    }
    uint8_t* rgba = NULL;

    // Bound the header before anything is allocated from it, so a corrupt
    // or hostile size field cannot ask for a huge decode buffer.
    spng_set_image_limits(ctx, SE_TEXTURE_MAX_DIM, SE_TEXTURE_MAX_DIM);
    int err = spng_set_png_file(ctx, f);
    struct spng_ihdr ihdr;
    if (err == 0) err = spng_get_ihdr(ctx, &ihdr);
    if (err != 0) {
        ESP_LOGE(TAG, "%s: not a PNG spng can read (%s)", path, spng_strerror(err));
        goto out;
    }
    if (!is_pow2(ihdr.width) || !is_pow2(ihdr.height)) {
        ESP_LOGE(TAG, "%s: %ux%u -- both edges must be a power of two", path, (unsigned)ihdr.width,
                 (unsigned)ihdr.height);
        goto out;
    }

    size_t size = 0;
    err = spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &size);
    if (err != 0) {
        ESP_LOGE(TAG, "%s: cannot size decode (%s)", path, spng_strerror(err));
        goto out;
    }
    // Scratch only: it lives for the length of this call, so PSRAM, where
    // it cannot compete with the texels for internal SRAM.
    rgba = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (rgba == NULL) {
        ESP_LOGE(TAG, "%s: no PSRAM for %u-byte decode scratch", path, (unsigned)size);
        goto out;
    }
    err = spng_decode_image(ctx, rgba, size, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS);
    if (err != 0) {
        ESP_LOGE(TAG, "%s: decode failed (%s)", path, spng_strerror(err));
        heap_caps_free(rgba);
        rgba = NULL;
        goto out;
    }
    *out_w = ihdr.width;
    *out_h = ihdr.height;

out:
    spng_ctx_free(ctx);
    return rgba;
}

se_texture_t* se_texture_load(char const* path, uint32_t flags) {
    if (path == NULL) return NULL;

    // Reserve the textured-triangle list up front: a texture with nowhere
    // to be drawn is not a texture worth loading, and doing it here keeps
    // the allocation at load time instead of in the middle of a frame.
    if (!scene_textured_reserve()) return NULL;

    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "%s: cannot open", path);
        return NULL;
    }
    uint32_t w = 0, h = 0;
    uint8_t* rgba = decode_rgba8(f, path, &w, &h);
    fclose(f);
    if (rgba == NULL) return NULL;

    size_t const   n       = (size_t)w * h;
    size_t const   bytes   = n * sizeof(uint16_t);
    uint16_t*      texels  = NULL;
    bool           in_sram = false;
    if (flags & SE_TEXTURE_INTERNAL) {
        texels  = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        in_sram = (texels != NULL);
        if (!in_sram) {
            ESP_LOGW(TAG, "%s: no internal SRAM for %u bytes (largest block %u) -- using PSRAM", path,
                     (unsigned)bytes, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
    }
    if (texels == NULL) texels = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    se_texture_t* tex = (texels != NULL) ? malloc(sizeof(*tex)) : NULL;
    if (tex == NULL) {
        ESP_LOGE(TAG, "%s: out of memory for %ux%u texture", path, (unsigned)w, (unsigned)h);
        heap_caps_free(texels);
        heap_caps_free(rgba);
        return NULL;
    }

    // RGBA8 -> RGB565 by truncation, the same rounding direct_565_pack
    // uses, so a texel and a flat triangle of the same ARGB come out as
    // the same pixel. The mean is taken on the 8-bit values, before that
    // truncation throws precision away.
    uint64_t sr = 0, sg = 0, sb = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t const r = rgba[4 * i + 0];
        uint8_t const g = rgba[4 * i + 1];
        uint8_t const b = rgba[4 * i + 2];
        sr += r;
        sg += g;
        sb += b;
        texels[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
    heap_caps_free(rgba);

    *tex = (se_texture_t){
        .texels    = texels,
        .w         = (int)w,
        .h         = (int)h,
        .w_log2    = log2_pow2(w),
        .internal  = in_sram,
        .mean_argb = 0xFF000000u | ((uint32_t)(sr / n) << 16) | ((uint32_t)(sg / n) << 8) | (uint32_t)(sb / n),
    };
    ESP_LOGI(TAG, "%s: %ux%u, %u bytes in %s", path, (unsigned)w, (unsigned)h, (unsigned)bytes,
             in_sram ? "internal SRAM" : "PSRAM");
    return tex;
}

void se_texture_unload(se_texture_t* tex) {
    if (tex == NULL) return;
    heap_caps_free(tex->texels);
    free(tex);
}
