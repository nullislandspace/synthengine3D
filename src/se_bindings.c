// =====================================================================
//  SynthEngine3D  --  input bindings + persistence (see se_bindings.h)
// ---------------------------------------------------------------------
//  Mirrors a game-declared control table into a current-scancode array,
//  loaded from / persisted to the game's NVS namespace (one u16 key per
//  control). The defs array is the game's (kept by reference); the
//  current values live here.
// =====================================================================

#include "se_bindings.h"

#include "se_config.h"   // SE_BINDINGS_MAX
#include "esp_log.h"
#include "nvs.h"

static char const TAG[] = "se_bindings";

static se_bindings_config_t s_cfg = {0};
static uint16_t             s_current[SE_BINDINGS_MAX];

// Linear scan over the (tiny) control table for `id`. Returns the slot
// index, or -1 if unknown. Bindings are few, so this is cheap even when
// polled per frame.
static int index_of(int id) {
    for (int i = 0; i < s_cfg.count; i++) {
        if (s_cfg.defs[i].id == id) return i;
    }
    return -1;
}

void se_bindings_init(se_bindings_config_t const* cfg) {
    if (cfg == NULL || cfg->defs == NULL) {
        s_cfg.count = 0;
        return;
    }
    s_cfg = *cfg;
    if (s_cfg.count > SE_BINDINGS_MAX) {
        ESP_LOGW(TAG, "%d controls declared, clamping to SE_BINDINGS_MAX=%d",
                 s_cfg.count, SE_BINDINGS_MAX);
        s_cfg.count = SE_BINDINGS_MAX;
    }

    // Start from the declared defaults so a missing key (or missing
    // namespace) leaves a sane value.
    for (int i = 0; i < s_cfg.count; i++) {
        s_current[i] = s_cfg.defs[i].default_sc;
    }

    nvs_handle_t h;
    esp_err_t    err = nvs_open(s_cfg.nvs_namespace, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;  // namespace doesn't exist yet -- defaults stand
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s) failed: %d (using defaults)", s_cfg.nvs_namespace, err);
        return;
    }
    for (int i = 0; i < s_cfg.count; i++) {
        uint16_t v = 0;
        if (nvs_get_u16(h, s_cfg.defs[i].nvs_key, &v) == ESP_OK && v != 0) {
            s_current[i] = v;
        }
    }
    nvs_close(h);
}

uint16_t se_bindings_get(int id) {
    int const i = index_of(id);
    return (i >= 0) ? s_current[i] : 0;
}

void se_bindings_set(int id, uint16_t sc) {
    if (sc == 0) return;
    int const i = index_of(id);
    if (i < 0 || s_current[i] == sc) return;
    s_current[i] = sc;

    nvs_handle_t h;
    if (nvs_open(s_cfg.nvs_namespace, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(%s, RW) failed", s_cfg.nvs_namespace);
        return;
    }
    if (nvs_set_u16(h, s_cfg.defs[i].nvs_key, sc) == ESP_OK) {
        nvs_commit(h);
    } else {
        ESP_LOGW(TAG, "nvs_set_u16(%s) failed", s_cfg.defs[i].nvs_key);
    }
    nvs_close(h);
}
