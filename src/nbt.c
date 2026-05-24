#include "se_nbt.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Byte-swap helpers
// ---------------------------------------------------------------------------

static uint16_t swap16(uint16_t v) {
    return (v >> 8) | (v << 8);
}

static uint32_t swap32(uint32_t v) {
    return ((v >> 24) & 0xFF) |
           ((v >>  8) & 0xFF00) |
           ((v <<  8) & 0xFF0000) |
           ((v << 24) & 0xFF000000);
}

static uint64_t swap64(uint64_t v) {
    return ((v >> 56) & 0xFFULL) |
           ((v >> 40) & 0xFF00ULL) |
           ((v >> 24) & 0xFF0000ULL) |
           ((v >>  8) & 0xFF000000ULL) |
           ((v <<  8) & 0xFF00000000ULL) |
           ((v << 24) & 0xFF0000000000ULL) |
           ((v << 40) & 0xFF000000000000ULL) |
           ((v << 56) & 0xFF00000000000000ULL);
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

static void w_bytes(NbtWriter* w, const void* data, size_t len) {
    if (w->error) return;
    if (fwrite(data, 1, len, w->f) != len) {
        w->error = 1;
    }
}

static void w_u8(NbtWriter* w, uint8_t v) {
    w_bytes(w, &v, 1);
}

static void w_u16(NbtWriter* w, uint16_t v) {
    w_bytes(w, &v, 2);
}

static void w_u32(NbtWriter* w, uint32_t v) {
    w_bytes(w, &v, 4);
}

static void w_i32(NbtWriter* w, int32_t v) {
    w_bytes(w, &v, 4);
}

static void w_i64(NbtWriter* w, int64_t v) {
    w_bytes(w, &v, 8);
}

static void w_f64(NbtWriter* w, double v) {
    w_bytes(w, &v, 8);
}

static void w_name(NbtWriter* w, const char* name) {
    uint16_t len = (uint16_t)strlen(name);
    w_u16(w, len);
    w_bytes(w, name, len);
}

void nbt_write_open(NbtWriter* w, FILE* f) {
    memset(w, 0, sizeof(*w));
    w->f = f;

    // Magic
    uint8_t magic[4] = { NBT_MAGIC_0, NBT_MAGIC_1, NBT_MAGIC_2, NBT_MAGIC_3 };
    w_bytes(w, magic, 4);

    // Endian sentinel
    uint32_t endian = NBT_ENDIAN_CHECK;
    w_u32(w, endian);

    // Version
    uint16_t version = NBT_FORMAT_VERSION;
    w_u16(w, version);
}

void nbt_write_compound(NbtWriter* w, const char* name) {
    w_u8(w, NBT_COMPOUND);
    w_name(w, name);
}

void nbt_write_end(NbtWriter* w) {
    w_u8(w, NBT_END);
}

void nbt_write_int32(NbtWriter* w, const char* name, int32_t value) {
    w_u8(w, NBT_INT32);
    w_name(w, name);
    w_i32(w, value);
}

void nbt_write_int64(NbtWriter* w, const char* name, int64_t value) {
    w_u8(w, NBT_INT64);
    w_name(w, name);
    w_i64(w, value);
}

void nbt_write_double(NbtWriter* w, const char* name, double value) {
    w_u8(w, NBT_DOUBLE);
    w_name(w, name);
    w_f64(w, value);
}

void nbt_write_string(NbtWriter* w, const char* name, const char* value) {
    w_u8(w, NBT_STRING);
    w_name(w, name);
    uint16_t len = (uint16_t)strlen(value);
    w_u16(w, len);
    w_bytes(w, value, len);
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

static int r_bytes(NbtReader* r, void* buf, size_t len) {
    if (r->error) return -1;
    if (fread(buf, 1, len, r->f) != len) {
        r->error = 1;
        return -1;
    }
    return 0;
}

static uint8_t r_u8(NbtReader* r) {
    uint8_t v = 0;
    r_bytes(r, &v, 1);
    return v;
}

static uint16_t r_u16(NbtReader* r) {
    uint16_t v = 0;
    r_bytes(r, &v, 2);
    if (r->swap) v = swap16(v);
    return v;
}

static int32_t r_i32(NbtReader* r) {
    int32_t v = 0;
    r_bytes(r, &v, 4);
    if (r->swap) {
        uint32_t u;
        memcpy(&u, &v, 4);
        u = swap32(u);
        memcpy(&v, &u, 4);
    }
    return v;
}

static int64_t r_i64(NbtReader* r) {
    int64_t v = 0;
    r_bytes(r, &v, 8);
    if (r->swap) {
        uint64_t u;
        memcpy(&u, &v, 8);
        u = swap64(u);
        memcpy(&v, &u, 8);
    }
    return v;
}

static double r_f64(NbtReader* r) {
    double v = 0;
    r_bytes(r, &v, 8);
    if (r->swap) {
        uint64_t u;
        memcpy(&u, &v, 8);
        u = swap64(u);
        memcpy(&v, &u, 8);
    }
    return v;
}

int nbt_read_open(NbtReader* r, FILE* f) {
    memset(r, 0, sizeof(*r));
    r->f = f;

    // Magic
    uint8_t magic[4];
    if (r_bytes(r, magic, 4) < 0) return -1;
    if (magic[0] != NBT_MAGIC_0 || magic[1] != NBT_MAGIC_1 ||
        magic[2] != NBT_MAGIC_2 || magic[3] != NBT_MAGIC_3) {
        r->error = 1;
        return -1;
    }

    // Endian sentinel — read raw, don't swap yet
    uint32_t endian = 0;
    if (r_bytes(r, &endian, 4) < 0) return -1;
    if (endian == NBT_ENDIAN_CHECK) {
        r->swap = 0;
    } else if (endian == swap32(NBT_ENDIAN_CHECK)) {
        r->swap = 1;
    } else {
        r->error = 1;
        return -1;
    }

    // Version
    r->version = r_u16(r);

    return r->error ? -1 : 0;
}

int nbt_peek_type(NbtReader* r) {
    if (r->error) return -1;
    int c = fgetc(r->f);
    if (c == EOF) return -1;
    ungetc(c, r->f);
    return c;
}

int nbt_read_tag(NbtReader* r, char* name_buf, int name_buf_size) {
    uint8_t type = r_u8(r);
    if (r->error) return -1;

    if (type == NBT_END) {
        if (name_buf && name_buf_size > 0) name_buf[0] = '\0';
        return NBT_END;
    }

    // Read name
    uint16_t name_len = r_u16(r);
    if (r->error) return -1;

    if (name_buf && name_buf_size > 0) {
        int copy = (name_len < name_buf_size - 1) ? name_len : name_buf_size - 1;
        if (r_bytes(r, name_buf, copy) < 0) return -1;
        name_buf[copy] = '\0';
        // Skip remaining name bytes if truncated
        int skip = name_len - copy;
        if (skip > 0) {
            fseek(r->f, skip, SEEK_CUR);
        }
    } else {
        fseek(r->f, name_len, SEEK_CUR);
    }

    return type;
}

int32_t nbt_read_int32(NbtReader* r) {
    return r_i32(r);
}

int64_t nbt_read_int64(NbtReader* r) {
    return r_i64(r);
}

double nbt_read_double(NbtReader* r) {
    return r_f64(r);
}

int nbt_read_string(NbtReader* r, char* buf, int buf_size) {
    uint16_t len = r_u16(r);
    if (r->error) return -1;

    int copy = (len < buf_size - 1) ? len : buf_size - 1;
    if (r_bytes(r, buf, copy) < 0) return -1;
    buf[copy] = '\0';

    // Skip remaining bytes if truncated
    int skip = len - copy;
    if (skip > 0) {
        fseek(r->f, skip, SEEK_CUR);
    }

    return len;
}

void nbt_skip_payload(NbtReader* r, int tag_type) {
    if (r->error) return;

    switch (tag_type) {
        case NBT_INT32:
            fseek(r->f, 4, SEEK_CUR);
            break;
        case NBT_INT64:
            fseek(r->f, 8, SEEK_CUR);
            break;
        case NBT_DOUBLE:
            fseek(r->f, 8, SEEK_CUR);
            break;
        case NBT_STRING: {
            uint16_t len = r_u16(r);
            if (!r->error) fseek(r->f, len, SEEK_CUR);
            break;
        }
        case NBT_COMPOUND: {
            // Skip all children until END
            char skip_name[256];
            int child_type;
            while ((child_type = nbt_read_tag(r, skip_name, sizeof(skip_name))) != NBT_END) {
                if (child_type < 0) break;
                nbt_skip_payload(r, child_type);
            }
            break;
        }
        default:
            r->error = 1;
            break;
    }
}
