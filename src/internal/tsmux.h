#pragma once
// =====================================================================
//  tsmux  --  H.264 access units -> MPEG-TS, 7 packets per UDP datagram
// ---------------------------------------------------------------------
//  Part C of claudeplans/nfmtest.md (D-02): what OBS's Media Source (an
//  ffmpeg input, udp://@:5000) plays without a server in between.
//
//    - PAT (PID 0) and PMT (PID 0x1000) before every keyframe, and at
//      least every TSMUX_TABLE_INTERVAL access units, so a receiver that
//      joins late finds the program quickly;
//    - H.264 on PID 0x100 (stream_type 0x1B), one PES per access unit,
//      with PTS (no B-frames, so DTS = PTS and is left out);
//    - the PCR on its OWN pid (TSMUX_PID_PCR), at a steady cadence set by
//      TSMUX_PCR_MAX_GAP rather than by the video frame rate: a receiver
//      recovers its clock from these, and a game's frame rate is far too
//      erratic to be a clock. The video's adaptation field carries only
//      the random access indicator now;
//    - an access unit delimiter in front of every access unit (the
//      encoder does not write one; ffmpeg's parser likes to have it);
//    - the SPS and PPS in front of every access unit that does not carry
//      its own, cached from the one that did. Without this a receiver can
//      only ever join in the first few milliseconds of a stream;
//    - random_access_indicator on keyframes;
//    - MPEG-2 Layer II audio on PID 0x101 (stream_type 0x04) when it is
//      switched on, one PES per audio frame, PTS only. The PCR stays on
//      the video PID: one clock, and the picture is the thing whose
//      timing a viewer notices.
//
//  Packets are gathered into datagrams of TSMUX_DGRAM_PACKETS x 188 =
//  1316 bytes, the usual size, one Ethernet frame each. The last
//  datagram of an access unit goes out short rather than waiting for the
//  next frame: latency matters more than the few bytes.
//
//  Plain C, no ESP headers: tools/tscheck.c tests it on the PC and
//  tools/tsmux_file.c muxes a whole .h264 for ffprobe.
// =====================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TSMUX_PACKET          188
#define TSMUX_DGRAM_PACKETS   7
#define TSMUX_DGRAM_MAX       (TSMUX_PACKET * TSMUX_DGRAM_PACKETS)  // 1316
#define TSMUX_PID_PMT         0x1000
#define TSMUX_PID_VIDEO       0x0100
#define TSMUX_PID_AUDIO       0x0101
// The clock gets its OWN pid. Carrying it in the video stream's adaptation
// field means clock packets land inside the video PES -- which is unbounded
// here (PES_packet_length 0), so a receiver treats everything on that pid
// until the next PES start as one packet, and ffmpeg calls every frame
// "Packet corrupt". A separate pid keeps the two entirely apart.
#define TSMUX_PID_PCR         0x0102
#define TSMUX_TABLE_INTERVAL  15      // access units between PAT/PMT at most
#define TSMUX_PARAMS_MAX      256     // room for one SPS + one PPS, with start codes
#define TSMUX_PCR_LEAD        9000    // 100 ms, in 90 kHz ticks
// Longest the clock may go unsampled. 13818-1 allows 100 ms and DVB asks
// for 40; this is 40, because the cost is one 188-byte packet.
#define TSMUX_PCR_MAX_GAP     3600    // 40 ms, in 90 kHz ticks

// Called for every finished datagram (1..7 packets); returns false if it
// could not be sent (counted, the muxer carries on).
typedef bool (*tsmux_emit_t)(void* ctx, uint8_t const* dgram, size_t len);

typedef struct {
    uint8_t      dgram[TSMUX_DGRAM_MAX];
    size_t       fill;
    uint8_t      cc_pat, cc_pmt, cc_video, cc_audio;  // continuity counters
    // The encoder sends SPS and PPS once, in its first access unit. Over
    // UDP anyone who joined later never sees them, so they are cached here
    // and put in front of every access unit that does not carry its own.
    uint8_t      params[TSMUX_PARAMS_MAX];
    size_t       params_len;
    bool         audio;                               // announce an audio stream in the PMT
    uint32_t     since_tables;
    tsmux_emit_t emit;
    void*        ctx;
    // statistics
    uint32_t     access_units;
    uint32_t     packets;
    uint32_t     dgrams;
    uint32_t     dgrams_failed;
    uint32_t     params_sent;  // access units given the cached SPS/PPS
    uint32_t     pcr_only;     // clock-only packets sent between frames
    uint64_t     pcr_last;     // value of the last PCR emitted
    bool         pcr_valid;
    uint64_t     bytes;        // datagram bytes handed to emit
} tsmux_t;

void tsmux_init(tsmux_t* m, tsmux_emit_t emit, void* ctx);

// Announce an audio stream in the PMT. Set before the first write: a
// receiver reads the PMT once and does not expect it to grow.
void tsmux_set_audio(tsmux_t* m, bool on);

// Mux one access unit: Annex-B NAL units (start codes included), as the
// encoder produced it. `pts` is in 90 kHz ticks. Ends with any partial
// datagram flushed.
void tsmux_write(tsmux_t* m, uint8_t const* au, size_t len, uint64_t pts, bool keyframe);

// Mux one MPEG-1 audio frame, whole, as the encoder produced it. `pts`
// is in 90 kHz ticks. No tables and no PCR: audio rides the clock the
// video already carries.
void tsmux_write_audio(tsmux_t* m, uint8_t const* frame, size_t len, uint64_t pts);

// Send a clock-only packet if none has gone out for TSMUX_PCR_MAX_GAP.
// `now_pts` is the PTS a frame captured at this instant would carry; the
// lead is subtracted here, as it is for a frame. Cheap and idempotent --
// call it every time round the stream loop.
void tsmux_pcr_if_due(tsmux_t* m, uint64_t now_pts);

// The MPEG-2 CRC32 of PSI sections (poly 0x04C11DB7, not reflected).
uint32_t tsmux_crc32(uint8_t const* data, size_t len);
