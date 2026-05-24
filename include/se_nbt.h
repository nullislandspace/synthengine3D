#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  NBT serialization
// ---------------------------------------------------------------------
//  A tiny tagged binary serializer (named/typed fields in nestable
//  compounds) over a stdio FILE*, with an endianness-tagged header so
//  files are portable. Used by se_save.h for slot files, but usable
//  standalone for any structured persistence. Part of the semver'd
//  public surface (see se_version.h).
// =====================================================================

#include <stdint.h>
#include <stdio.h>

// Tag type IDs
#define NBT_END      0x00
#define NBT_INT32    0x01
#define NBT_DOUBLE   0x02
#define NBT_STRING   0x03
#define NBT_COMPOUND 0x04
#define NBT_INT64    0x05

// File header constants
#define NBT_MAGIC_0       'S'
#define NBT_MAGIC_1       'Y'
#define NBT_MAGIC_2       'N'
#define NBT_MAGIC_3       'T'
#define NBT_ENDIAN_CHECK  0x01020304
#define NBT_FORMAT_VERSION 1

// --- Writer ---

typedef struct {
    FILE* f;
    int   error;      // non-zero if any write failed
    int   swap;       // non-zero if byte-swapping needed (always 0 when writing)
} NbtWriter;

void nbt_write_open(NbtWriter* w, FILE* f);
void nbt_write_compound(NbtWriter* w, const char* name);
void nbt_write_end(NbtWriter* w);
void nbt_write_int32(NbtWriter* w, const char* name, int32_t value);
void nbt_write_int64(NbtWriter* w, const char* name, int64_t value);
void nbt_write_double(NbtWriter* w, const char* name, double value);
void nbt_write_string(NbtWriter* w, const char* name, const char* value);

// --- Reader ---

typedef struct {
    FILE*    f;
    int      error;
    int      swap;     // non-zero if file endianness differs from host
    uint16_t version;  // format version from header
} NbtReader;

int  nbt_read_open(NbtReader* r, FILE* f);
int  nbt_peek_type(NbtReader* r);
int  nbt_read_tag(NbtReader* r, char* name_buf, int name_buf_size);
int32_t nbt_read_int32(NbtReader* r);
int64_t nbt_read_int64(NbtReader* r);
double  nbt_read_double(NbtReader* r);
int     nbt_read_string(NbtReader* r, char* buf, int buf_size);
void    nbt_skip_payload(NbtReader* r, int tag_type);
