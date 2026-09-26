// =====================================================================
//  tsmux  --  H.264 access units -> MPEG-TS (see tsmux.h)
// =====================================================================

#include "tsmux.h"
#include <string.h>

#define PID_PAT        0x0000
#define PROGRAM_NUMBER 1
#define STREAM_H264    0x1B
// 0x04 is ISO/IEC 13818-3 audio, which is what MPEG-2 LSF Layer II at
// 22050 Hz is. 0x03 (11172-3, MPEG-1) also plays in practice because
// both map to the same decoder, but the header we emit says LSF and the
// PMT should not disagree with it.
#define STREAM_MPEG2_AUDIO 0x04

// An access unit delimiter: nal_unit_type 9, primary_pic_type 7 ("any"),
// then the rbsp stop bit.
static uint8_t const AUD[] = {0x00, 0x00, 0x00, 0x01, 0x09, 0xF0};

// Is there a start code at `i`? Annex-B allows three bytes or four.
static size_t start_code_at(uint8_t const* d, size_t len, size_t i) {
    if (i + 3 > len) return 0;
    if (d[i] != 0 || d[i + 1] != 0) return 0;
    if (d[i + 2] == 1) return 3;
    if (d[i + 2] == 0 && i + 4 <= len && d[i + 3] == 1) return 4;
    return 0;
}

// WHY THE PARAMETER SETS ARE RESENT WITH EVERY ACCESS UNIT.
//
// The encoder emits SPS and PPS once, in its first access unit. Over UDP
// that is the same as not emitting them at all for anyone who was not
// already listening: there is no connection, no handshake and no way to
// ask for a replay, so a receiver that opens its socket a second late has
// missed them for good and every slice after that references a PPS it has
// never seen. The symptom is not silence -- the transport stream parses,
// the program is found, the stream is correctly identified as H.264 -- it
// is "unspecified size" and "non-existing PPS 0 referenced" for ever.
//
// Resending before each KEYFRAME is the usual answer and is not enough
// here. OBS opens its media source with analyzeduration 0, so it decides
// what the stream contains from the first few packets it happens to see
// and does not wait a GOP to find out. ffplay, which waits, plays the
// same stream perfectly -- which is exactly how this was mistaken for a
// working stream.
//
// So they go in front of every access unit. SPS and PPS together are
// about forty bytes; at twenty frames a second that is under a kilobyte a
// second against a three megabit stream, and it buys a stream any
// receiver can join at any instant.
static void cache_params(tsmux_t* m, uint8_t const* au, size_t len, bool* has_sps) {
    size_t sps_at = 0, sps_end = 0, pps_at = 0, pps_end = 0;
    bool   sps = false, pps = false;
    size_t i = 0;

    while (i < len) {
        size_t const sc = start_code_at(au, len, i);
        if (sc == 0) { i++; continue; }
        size_t const nal = i + sc;
        if (nal >= len) break;
        int const type = au[nal] & 0x1F;
        size_t    j    = nal + 1;
        while (j < len && start_code_at(au, len, j) == 0) j++;
        if (type == 7) { sps_at = i; sps_end = j; sps = true; }
        else if (type == 8) { pps_at = i; pps_end = j; pps = true; }
        i = j;
    }

    *has_sps = sps;
    if (sps && pps) {
        size_t const n = (sps_end - sps_at) + (pps_end - pps_at);
        if (n <= TSMUX_PARAMS_MAX) {
            memcpy(m->params, au + sps_at, sps_end - sps_at);
            memcpy(m->params + (sps_end - sps_at), au + pps_at, pps_end - pps_at);
            m->params_len = n;
        }
    }
}

uint32_t tsmux_crc32(uint8_t const* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int b = 0; b < 8; b++) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

void tsmux_set_audio(tsmux_t* m, bool on) {
    m->audio = on;
}

void tsmux_init(tsmux_t* m, tsmux_emit_t emit, void* ctx) {
    memset(m, 0, sizeof(*m));
    m->emit         = emit;
    m->ctx          = ctx;
    m->since_tables = TSMUX_TABLE_INTERVAL;  // tables go out with the first access unit
}

static void flush(tsmux_t* m) {
    if (m->fill == 0) return;
    m->dgrams++;
    m->bytes += m->fill;
    if (!m->emit(m->ctx, m->dgram, m->fill)) m->dgrams_failed++;
    m->fill = 0;
}

// A fresh 188-byte packet in the datagram, full datagrams sent first.
static uint8_t* next_packet(tsmux_t* m) {
    if (m->fill + TSMUX_PACKET > sizeof(m->dgram)) flush(m);
    uint8_t* p = m->dgram + m->fill;
    m->fill += TSMUX_PACKET;
    m->packets++;
    return p;
}

static void header(uint8_t* p, uint16_t pid, bool pusi, uint8_t afc, uint8_t cc) {
    p[0] = 0x47;
    p[1] = (uint8_t)((pusi ? 0x40 : 0) | ((pid >> 8) & 0x1F));
    p[2] = (uint8_t)pid;
    p[3] = (uint8_t)((afc << 4) | (cc & 0x0F));
}

// One PSI section in one packet (both tables are tiny): pointer field,
// section, 0xFF padding.
static void section_packet(tsmux_t* m, uint16_t pid, uint8_t* cc, uint8_t const* sec, size_t len) {
    uint8_t* p = next_packet(m);
    header(p, pid, true, 1, (*cc)++);
    p[4] = 0;  // pointer_field
    memcpy(p + 5, sec, len);
    memset(p + 5 + len, 0xFF, TSMUX_PACKET - 5 - len);
}

static void write_tables(tsmux_t* m) {
    // PAT: one program, its PMT on TSMUX_PID_PMT.
    uint8_t pat[16];
    size_t  n = 0;
    pat[n++]  = 0x00;  // table_id
    pat[n++]  = 0xB0;  // section_syntax_indicator, '0', reserved, length hi = 0
    pat[n++]  = 13;    // section_length: 5 header + 4 program + 4 CRC
    pat[n++]  = 0x00;
    pat[n++]  = 0x01;  // transport_stream_id 1
    pat[n++]  = 0xC1;  // version 0, current_next 1
    pat[n++]  = 0x00;  // section_number
    pat[n++]  = 0x00;  // last_section_number
    pat[n++]  = PROGRAM_NUMBER >> 8;
    pat[n++]  = PROGRAM_NUMBER & 0xFF;
    pat[n++]  = (uint8_t)(0xE0 | (TSMUX_PID_PMT >> 8));
    pat[n++]  = TSMUX_PID_PMT & 0xFF;
    uint32_t crc = tsmux_crc32(pat, n);
    pat[n++]     = (uint8_t)(crc >> 24);
    pat[n++]     = (uint8_t)(crc >> 16);
    pat[n++]     = (uint8_t)(crc >> 8);
    pat[n++]     = (uint8_t)crc;
    section_packet(m, PID_PAT, &m->cc_pat, pat, n);

    // PMT: PCR and H.264 on the video PID, and the audio beside it.
    uint8_t pmt[26];
    n        = 0;
    pmt[n++] = 0x02;  // table_id
    pmt[n++] = 0xB0;
    pmt[n++] = (uint8_t)(m->audio ? 23 : 18);  // 9 header + 5 per stream + 4 CRC
    pmt[n++] = PROGRAM_NUMBER >> 8;
    pmt[n++] = PROGRAM_NUMBER & 0xFF;
    pmt[n++] = 0xC1;
    pmt[n++] = 0x00;
    pmt[n++] = 0x00;
    pmt[n++] = (uint8_t)(0xE0 | (TSMUX_PID_VIDEO >> 8));  // PCR_PID
    pmt[n++] = TSMUX_PID_VIDEO & 0xFF;
    pmt[n++] = 0xF0;  // program_info_length 0
    pmt[n++] = 0x00;
    pmt[n++] = STREAM_H264;
    pmt[n++] = (uint8_t)(0xE0 | (TSMUX_PID_VIDEO >> 8));
    pmt[n++] = TSMUX_PID_VIDEO & 0xFF;
    pmt[n++] = 0xF0;  // ES_info_length 0
    pmt[n++] = 0x00;
    if (m->audio) {
        pmt[n++] = STREAM_MPEG2_AUDIO;
        pmt[n++] = (uint8_t)(0xE0 | (TSMUX_PID_AUDIO >> 8));
        pmt[n++] = TSMUX_PID_AUDIO & 0xFF;
        pmt[n++] = 0xF0;  // ES_info_length 0
        pmt[n++] = 0x00;
    }
    crc      = tsmux_crc32(pmt, n);
    pmt[n++] = (uint8_t)(crc >> 24);
    pmt[n++] = (uint8_t)(crc >> 16);
    pmt[n++] = (uint8_t)(crc >> 8);
    pmt[n++] = (uint8_t)crc;
    section_packet(m, TSMUX_PID_PMT, &m->cc_pmt, pmt, n);
}

// A 33-bit PTS in the 5-byte PES form, with prefix '0010'.
static void put_pts(uint8_t* p, uint64_t pts) {
    pts &= 0x1FFFFFFFFull;
    p[0] = (uint8_t)(0x21 | ((pts >> 29) & 0x0E));
    p[1] = (uint8_t)(pts >> 22);
    p[2] = (uint8_t)(0x01 | ((pts >> 14) & 0xFE));
    p[3] = (uint8_t)(pts >> 7);
    p[4] = (uint8_t)(0x01 | ((pts << 1) & 0xFE));
}

// Copies payload from the PES header, then the AUD, then the access unit,
// as if they were one buffer.
typedef struct {
    uint8_t const* part[4];
    size_t         len[4];
    int            idx;
    size_t         off;
} src_t;

static size_t src_left(src_t const* s) {
    size_t n = 0;
    for (int i = s->idx; i < 4; i++) n += s->len[i] - (i == s->idx ? s->off : 0);
    return n;
}

static void src_copy(src_t* s, uint8_t* out, size_t n) {
    while (n > 0) {
        size_t const avail = s->len[s->idx] - s->off;
        size_t const k     = avail < n ? avail : n;
        memcpy(out, s->part[s->idx] + s->off, k);
        out += k;
        n -= k;
        s->off += k;
        if (s->off == s->len[s->idx]) {
            s->idx++;
            s->off = 0;
        }
    }
}

void tsmux_write(tsmux_t* m, uint8_t const* au, size_t len, uint64_t pts, bool keyframe) {
    if (keyframe || m->since_tables >= TSMUX_TABLE_INTERVAL) {
        write_tables(m);
        m->since_tables = 0;
    }
    m->since_tables++;
    m->access_units++;

    // PES header: video stream 0xE0, unbounded length (allowed for video
    // in TS), PTS only.
    uint8_t pes[14];
    pes[0] = 0x00;
    pes[1] = 0x00;
    pes[2] = 0x01;
    pes[3] = 0xE0;
    pes[4] = 0x00;
    pes[5] = 0x00;  // PES_packet_length 0: unbounded
    pes[6] = 0x80;  // '10', no scrambling, no priority, no alignment flag, not copyright, copy
    pes[7] = 0x80;  // PTS only
    pes[8] = 5;     // PES_header_data_length
    put_pts(pes + 9, pts);

    // Cache this access unit's parameter sets if it carries them, and put
    // the cached ones in front of it if it does not. See cache_params().
    bool has_sps = false;
    cache_params(m, au, len, &has_sps);
    uint8_t const* inject     = m->params;
    size_t         inject_len = (!has_sps && m->params_len > 0) ? m->params_len : 0;
    if (inject_len) m->params_sent++;

    src_t src = {.part = {pes, AUD, inject, au},
                 .len  = {sizeof(pes), sizeof(AUD), inject_len, len}};

    bool first = true;
    while (src_left(&src) > 0) {
        size_t const left = src_left(&src);
        uint8_t*     p    = next_packet(m);
        // Adaptation field: PCR (+ random access) in the first packet;
        // stuffing in the last one if the payload does not fill it.
        size_t af_len = 0;  // bytes after the adaptation_field_length byte
        bool   has_af = false;
        if (first) {
            has_af = true;
            af_len = 1 + 6;  // flags + PCR
        }
        size_t room = TSMUX_PACKET - 4 - (has_af ? 1 + af_len : 0);
        if (left < room) {
            // Pad with adaptation field stuffing so the payload ends the packet.
            size_t const pad = room - left;
            if (!has_af) {
                has_af = true;
                af_len = pad - 1;  // the length byte itself takes one
                if (pad == 1) af_len = 0;
            } else {
                af_len += pad;
            }
            room = left;
        }
        header(p, TSMUX_PID_VIDEO, first, has_af ? 3 : 1, m->cc_video++);
        size_t o = 4;
        if (has_af) {
            p[o++] = (uint8_t)af_len;
            if (af_len > 0) {
                size_t const af_start = o;
                uint8_t      flags    = 0;
                if (first) flags |= 0x10 | (keyframe ? 0x40 : 0);  // PCR_flag, random_access_indicator
                p[o++] = flags;
                if (first) {
                    uint64_t const base = (pts - TSMUX_PCR_LEAD) & 0x1FFFFFFFFull;
                    p[o++]              = (uint8_t)(base >> 25);
                    p[o++]              = (uint8_t)(base >> 17);
                    p[o++]              = (uint8_t)(base >> 9);
                    p[o++]              = (uint8_t)(base >> 1);
                    p[o++]              = (uint8_t)(((base & 1) << 7) | 0x7E);  // reserved 6 bits, extension hi
                    p[o++]              = 0x00;                                 // extension lo
                }
                memset(p + o, 0xFF, af_start + af_len - o);
                o = af_start + af_len;
            }
        }
        src_copy(&src, p + o, TSMUX_PACKET - o);
        first = false;
    }
    flush(m);  // the rest of this access unit goes now, not with the next one
}

// --- audio ---------------------------------------------------------------
//
// Simpler than the video above: one frame is at most a couple of
// kilobytes, there is no PCR to carry and no access unit delimiter to
// prepend, and the PES length is KNOWN -- which for audio it must be,
// since only video may leave it unbounded.
void tsmux_write_audio(tsmux_t* m, uint8_t const* frame, size_t len, uint64_t pts) {
    if (frame == NULL || len == 0) return;

    uint8_t      pes[14];
    size_t const pes_len = 3 + 5 + len;  // flags + header length byte + PTS + payload
    pes[0]               = 0x00;
    pes[1]               = 0x00;
    pes[2]               = 0x01;
    pes[3]               = 0xC0;  // audio stream 0
    pes[4]               = (uint8_t)(pes_len >> 8);
    pes[5]               = (uint8_t)pes_len;
    pes[6]               = 0x80;  // '10', no scrambling
    pes[7]               = 0x80;  // PTS only
    pes[8]               = 5;     // PES_header_data_length
    put_pts(pes + 9, pts);

    src_t src = {.part = {pes, frame, NULL}, .len = {sizeof(pes), len, 0}};

    bool first = true;
    while (src_left(&src) > 0) {
        size_t const left   = src_left(&src);
        uint8_t*     p      = next_packet(m);
        size_t       af_len = 0;
        bool         has_af = false;
        size_t       room   = TSMUX_PACKET - 4;
        if (left < room) {
            // Stuffing, so the payload ends the packet rather than being
            // split across one that has nothing to follow it.
            size_t const pad = room - left;
            has_af           = true;
            af_len           = pad == 1 ? 0 : pad - 1;
            room             = left;
        }
        header(p, TSMUX_PID_AUDIO, first, has_af ? 3 : 1, m->cc_audio++);
        size_t o = 4;
        if (has_af) {
            p[o++] = (uint8_t)af_len;
            if (af_len > 0) {
                p[o++] = 0x00;  // no flags
                memset(p + o, 0xFF, af_len - 1);
                o += af_len - 1;
            }
        }
        src_copy(&src, p + o, TSMUX_PACKET - o);
        first = false;
    }
    flush(m);
}
