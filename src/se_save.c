#include "se_save.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_log.h"
#include "fastopen.h"

static char const* TAG = "se_save";

// Bump if the peek block's layout changes incompatibly. Stored in every
// peek as "fmt"; surfaced to the game in se_save_peek_t.format_version.
#define SE_SAVE_PEEK_VERSION 1

static se_save_config_t s_cfg;
static bool             s_inited = false;

static void slot_path(int slot, char* buf, size_t size) {
    snprintf(buf, size, "%s/save%d.bin", s_cfg.dir ? s_cfg.dir : ".", slot);
}

void se_save_init(se_save_config_t const* cfg) {
    if (!cfg) return;
    s_cfg    = *cfg;
    s_inited = true;

    if (s_cfg.dir) {
        struct stat st;
        if (stat(s_cfg.dir, &st) != 0) {
            if (mkdir(s_cfg.dir, 0755) != 0) {
                ESP_LOGE(TAG, "mkdir %s failed", s_cfg.dir);
            } else {
                ESP_LOGI(TAG, "created %s", s_cfg.dir);
            }
        }
    }
}

int se_save_slot_exists(int slot) {
    if (slot < 0 || slot >= SE_SAVE_SLOT_COUNT) return 0;
    char path[96];
    slot_path(slot, path, sizeof(path));
    struct stat st;
    return stat(path, &st) == 0;
}

// Engine-owned peek block, written first inside the root compound.
static void write_peek(NbtWriter* w, se_save_kind_t kind, char const* info) {
    nbt_write_compound(w, "se_peek");
    nbt_write_int64 (w, "ts",    (int64_t)time(NULL));
    nbt_write_int32 (w, "fmt",   SE_SAVE_PEEK_VERSION);
    nbt_write_int32 (w, "kind",  (int32_t)kind);
    nbt_write_int32 (w, "gver",  s_cfg.game_version);
    nbt_write_string(w, "gname", s_cfg.game_name ? s_cfg.game_name : "");
    nbt_write_string(w, "info",  info ? info : "");
    nbt_write_end(w);
}

static void read_peek_fields(NbtReader* r, se_save_peek_t* out) {
    char name[64];
    int  type;
    while ((type = nbt_read_tag(r, name, sizeof(name))) != NBT_END) {
        if (type < 0) break;
        if      (type == NBT_INT64  && !strcmp(name, "ts"))    out->timestamp      = nbt_read_int64(r);
        else if (type == NBT_INT32  && !strcmp(name, "fmt"))   out->format_version = nbt_read_int32(r);
        else if (type == NBT_INT32  && !strcmp(name, "kind"))  out->kind           = (se_save_kind_t)nbt_read_int32(r);
        else if (type == NBT_INT32  && !strcmp(name, "gver"))  out->game_version   = nbt_read_int32(r);
        else if (type == NBT_STRING && !strcmp(name, "gname")) nbt_read_string(r, out->game_name, (int)sizeof(out->game_name));
        else if (type == NBT_STRING && !strcmp(name, "info"))  nbt_read_string(r, out->info, (int)sizeof(out->info));
        else nbt_skip_payload(r, type);
    }
}

int se_save_peek(int slot, se_save_peek_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (slot < 0 || slot >= SE_SAVE_SLOT_COUNT) return -1;

    char path[96];
    slot_path(slot, path, sizeof(path));

    FILE* f = fastopen(path, "rb");
    if (!f) return -1;

    NbtReader r;
    if (nbt_read_open(&r, f) < 0) { fastclose(f); return -1; }

    char name[64];
    int  type = nbt_read_tag(&r, name, sizeof(name));   // root compound
    if (type != NBT_COMPOUND) { fastclose(f); return -1; }
    out->exists = true;

    // The engine always writes se_peek first. If it isn't there (a slot
    // from an older format), leave the peek fields at their defaults.
    type = nbt_read_tag(&r, name, sizeof(name));
    if (type == NBT_COMPOUND && strcmp(name, "se_peek") == 0) {
        read_peek_fields(&r, out);
    }

    fastclose(f);
    return r.error ? -1 : 0;
}

int se_save_load_slot(int slot, void* game_data) {
    if (!s_inited || !s_cfg.deserialize) return -1;
    if (slot < 0 || slot >= SE_SAVE_SLOT_COUNT) return -1;

    char path[96];
    slot_path(slot, path, sizeof(path));

    FILE* f = fastopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "no save file at %s", path);
        return -1;
    }

    NbtReader r;
    if (nbt_read_open(&r, f) < 0) {
        ESP_LOGE(TAG, "bad header in %s", path);
        fastclose(f);
        return -1;
    }

    char name[64];
    int  type = nbt_read_tag(&r, name, sizeof(name));   // root compound
    if (type != NBT_COMPOUND) { fastclose(f); return -1; }

    // Game reads its own compounds, skipping the engine's se_peek block.
    s_cfg.deserialize(&r, game_data);
    fastclose(f);

    if (r.error) {
        ESP_LOGE(TAG, "read error in %s", path);
        return -1;
    }
    ESP_LOGI(TAG, "loaded slot %d (v%d, swap=%d)", slot, r.version, r.swap);
    return 0;
}

int se_save_write_slot(int slot, se_save_kind_t kind,
                       void const* game_data, char const* info) {
    if (!s_inited || !s_cfg.serialize) return -1;
    if (slot < 0 || slot >= SE_SAVE_SLOT_COUNT) return -1;

    char path[96];
    slot_path(slot, path, sizeof(path));

    FILE* f = fastopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s for write failed", path);
        return -1;
    }

    NbtWriter w;
    nbt_write_open(&w, f);
    nbt_write_compound(&w, "root");
    write_peek(&w, kind, info);          // engine-owned peek header first
    s_cfg.serialize(&w, game_data);      // then the game's own state
    nbt_write_end(&w);
    fastclose(f);

    if (w.error) {
        ESP_LOGE(TAG, "write error to %s", path);
        return -1;
    }
    ESP_LOGI(TAG, "saved slot %d", slot);
    return 0;
}
