#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  save-slot framework
// ---------------------------------------------------------------------
//  Generic file-backed save slots. The engine owns the slot files, the
//  on-disk wrapping (NBT root + a peek header), and the slot directory;
//  the GAME owns its save schema and (de)serialises it through two
//  callbacks. A "peek" header is written at the front of every slot so
//  a slot-select screen can show date / kind / a free-text summary
//  without loading the full game state.
//
//  The engine fills the peek's timestamp + format version automatically;
//  the game supplies its name + version (once, in the config) and a
//  display `info` string + save kind (per write). Part of the semver'd
//  public surface (see se_version.h).
// =====================================================================

#include "se_config.h"   // SE_SAVE_SLOT_COUNT (overridable)
#include "se_nbt.h"

#include <stdbool.h>
#include <stdint.h>

#define SE_SAVE_GAME_NAME_MAX 32
#define SE_SAVE_INFO_MAX      64

// How a save was produced. Stored in the peek; the engine assigns no
// behaviour to it — it's metadata for the game's UI. A game that only
// ever saves manually just always passes SE_SAVE_MANUAL.
typedef enum {
    SE_SAVE_MANUAL    = 0,
    SE_SAVE_AUTOSAVE  = 1,
    SE_SAVE_QUICKSAVE = 2,
} se_save_kind_t;

// Peek header — read without loading the full game state. The engine
// fills `exists`, `timestamp`, and `format_version`; the rest is the
// metadata the game supplied at write time.
typedef struct {
    bool           exists;          // true if the slot file opened OK
    int64_t        timestamp;       // engine: time(NULL) at save (0 = none)
    int32_t        format_version;  // engine: se_save peek format version
    se_save_kind_t kind;            // game-set at write
    int32_t        game_version;    // game-set (from config)
    char           game_name[SE_SAVE_GAME_NAME_MAX];  // game-set (config)
    char           info[SE_SAVE_INFO_MAX];            // game-set display text
} se_save_peek_t;

// Schema callbacks. The game (de)serialises ONLY its own state through
// the NBT writer/reader; the engine has already written the root
// compound + peek header (writer) or consumed the root tag (reader).
// The deserialiser must skip tags it doesn't recognise (the engine's
// peek compound is one such tag) — nbt_skip_payload() handles that, and
// the typical "loop tags, dispatch known, skip rest" reader already does.
typedef void (*se_save_serialize_fn)(NbtWriter* w, void const* game_data);
typedef void (*se_save_deserialize_fn)(NbtReader* r, void* game_data);

typedef struct {
    char const*            dir;          // slot directory, e.g. "/int/mygame"
    char const*            game_name;    // stamped into every peek
    int32_t                game_version; // stamped into every peek
    se_save_serialize_fn   serialize;    // required
    se_save_deserialize_fn deserialize;  // required
} se_save_config_t;

// Register the config (copied internally) and create the slot directory
// if missing. Call once at boot before any other se_save_* call.
void se_save_init(se_save_config_t const* cfg);

// 1 if the slot file exists, 0 otherwise (also 0 for out-of-range slot).
int se_save_slot_exists(int slot);

// Read just the peek header into `out` (zeroed first). Returns 0 on
// success, -1 on missing / corrupt / IO error. A slot saved by an older
// format without a peek block still returns 0 with `exists=true` and the
// peek fields left at their defaults.
int se_save_peek(int slot, se_save_peek_t* out);

// Load a slot: opens the file, consumes the NBT root, then calls the
// configured deserialize() to fill `game_data`. The game is responsible
// for defaulting `game_data` before calling (so missing tags keep their
// defaults). Returns 0 on success, -1 on missing / corrupt / IO error.
int se_save_load_slot(int slot, void* game_data);

// Write a slot: stamps timestamp + format version, records `kind`, the
// configured game name/version, and the `info` display string into the
// peek header, then calls serialize() for the game state. `info` may be
// NULL (treated as empty). Returns 0 on success, -1 on IO error.
int se_save_write_slot(int slot, se_save_kind_t kind,
                       void const* game_data, char const* info);
