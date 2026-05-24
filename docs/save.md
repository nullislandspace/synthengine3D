# Save framework (`se_save.h`, `se_nbt.h`)

N file-backed save slots. The **engine** owns the slot files, the slot
directory, the on-disk wrapping (an NBT root + a "peek" header), and reading
that header cheaply for a slot-select screen. The **game** owns its save schema
and (de)serialises it through two callbacks. The engine assigns no meaning to
your data — it just frames it.

## Setup

```c
static void serialize(NbtWriter* w, void const* game_data) {
    save_data_t const* s = game_data;
    nbt_write_int32(w, "level", s->level);
    nbt_write_int64(w, "highscore", s->highscore);
    // ... your fields ...
}
static void deserialize(NbtReader* r, void* game_data) {
    save_data_t* s = game_data;
    // loop tags, dispatch the ones you know, skip the rest (see below)
}

se_save_config_t const cfg = {
    .dir          = "/int/mygame",   // slot directory (engine mkdir's it)
    .game_name    = "My Game",        // stamped into every peek
    .game_version = 1,
    .serialize    = serialize,        // required
    .deserialize  = deserialize,      // required
};
se_save_init(&cfg);                   // once at boot, before any other se_save_*
```

`SE_SAVE_SLOT_COUNT` (in [`se_config.h`](configuration.md)) sets how many slots
exist (default 3).

## Reading / writing

```c
int se_save_slot_exists(int slot);                          // 1 / 0
int se_save_peek(int slot, se_save_peek_t* out);            // header only
int se_save_load_slot(int slot, void* game_data);           // calls deserialize
int se_save_write_slot(int slot, se_save_kind_t kind,
                       void const* game_data, char const* info);
```

`write` stamps `timestamp` (engine: `time(NULL)`) + the peek format version,
records `kind`, your config's name/version, and the free-text `info` string,
then calls your `serialize`. `load` consumes the NBT root then calls your
`deserialize`. **Default `game_data` before loading** so tags missing from an
older save keep their defaults.

`load` / `peek` return `-1` on missing / corrupt / IO error (so a fresh slot
just reads as "absent" — start a new profile).

## The peek header — for slot-select

`se_save_peek()` reads only the header, without loading full state, so a
slot-select screen can list dates and summaries fast:

```c
typedef struct {
    bool           exists;
    int64_t        timestamp;       // engine-set
    int32_t        format_version;  // engine-set
    se_save_kind_t kind;            // game-set at write (MANUAL/AUTOSAVE/QUICKSAVE)
    int32_t        game_version;    // game-set (config)
    char           game_name[SE_SAVE_GAME_NAME_MAX];
    char           info[SE_SAVE_INFO_MAX];   // your display summary
} se_save_peek_t;
```

`info` is whatever you want shown ("stage 3 · 12000 pts"); `kind` is metadata
for your UI (a manual-only game always passes `SE_SAVE_MANUAL`). A slot written
by an older format with no peek block still returns `exists = true` with the
peek fields at defaults — show a blank date until it's re-saved.

## The deserialiser must skip unknown tags

The engine writes its own `se_peek` compound first in the file; your reader
will encounter it as an unknown tag. The standard "loop tags, dispatch known,
skip the rest" reader handles this — use `nbt_skip_payload()` for anything you
don't recognise. This also future-proofs the format: new tags an old build
doesn't know are skipped, not fatal.

## NBT primitive (`se_nbt.h`)

The save files sit on a tiny tagged binary serializer — named/typed fields
(`int32`, `int64`, `double`, `string`, nestable `compound`) over a stdio
`FILE*`, with an endianness-tagged header so files are portable across the
device's and a host's byte order. It's usable standalone for any structured
persistence, not just save slots. See the header for the writer/reader API
(`nbt_write_*` / the reader's tag-walk + `nbt_skip_payload`).
