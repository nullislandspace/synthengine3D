#pragma once
// =====================================================================
//  SynthEngine3D  --  PUBLIC STABLE API  --  live A/V streaming to a PC
// ---------------------------------------------------------------------
//  The game's screen, and its sound, out of the USB-C port as MPEG-TS
//  over UDP. OBS plays it directly, with no server in between. Part of
//  the versioned public surface (see se_version.h).
//
//      se_stream_start()   brings up the link and the encoders
//      se_stream_frame()   offer a finished frame, once per frame
//      se_stream_stop()    puts the port back
//
//  On the PC: OBS -> Sources -> Media Source, untick Local File, input
//  `udp://@:5000`, format `mpegts`. Or `ffplay -fflags nobuffer
//  udp://@:5000`. The picture appears within about a second, because the
//  player waits for the next keyframe.
//
//  THE CONSOLE GOES AWAY WHILE THIS RUNS, and that is not a detail. The
//  USB-C port has one PHY: starting the stream takes it off the
//  USB-Serial-JTAG console and hands it to the OTG controller, so for as
//  long as the stream is up there is no console, no debug link and no
//  log output at all. Whatever switches this on must therefore be
//  reachable WITHOUT a console -- a key, a menu row -- and a game should
//  not persist the setting, or a badge can be locked out of its own
//  development link across a restart with nothing on screen to say why.
//
//  AUDIO COMES FOR FREE, because the mixer is the engine's own
//  (se_audio.h): there is nothing for the game to wire up and no tap for
//  it to hold. `cfg.audio` mixes what the player hears into the same
//  transport stream, in sync with the picture.
//
//  WHAT IT COSTS THE FRAME. se_stream_frame() converts the frame to
//  YUV420 on the PPA and returns; the encoder, the muxer and the USB are
//  another task's work, off the frame path. A frame offered while the
//  encoder is still busy is DROPPED and counted, never waited for, so a
//  stream that cannot keep up stutters instead of slowing the game down.
//
//  Ported from tanmatsu-nfmtest-grace, which was written to find out
//  whether this was possible at all and what it cost.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pax_gfx.h"

typedef struct {
    // Video target. 0 -> 3000 kbit/s, which is ample for 800x480 at the
    // rate a game on this badge actually renders.
    uint32_t bitrate_kbit;
    // Frames between keyframes. 0 -> `fps_hint`, so about one a second.
    // A player joining mid-stream waits for the next one.
    int gop;
    // What the encoder's rate control and the stream clock are scaled
    // against. It is a HINT, not a promise: the game offers frames when
    // it has them and there is no fixed rate. 0 -> 30.
    int fps_hint;
    // Mix the engine's own audio into the stream. Costs one software
    // encoder on the mixer's task; see the note above.
    bool audio;
} se_stream_cfg_t;

typedef struct {
    uint32_t published;      // frames offered by the game
    uint32_t dropped;        // offered while the encoder was busy
    uint32_t frames;         // encoded and muxed
    uint32_t keyframes;
    uint32_t enc_errors;
    uint32_t ppa_errors;
    uint64_t es_bytes;       // H.264 bytes out of the encoder
    uint32_t audio_frames;   // MPEG audio frames muxed
    uint32_t audio_dropped;  // PCM frames overwritten before they were encoded
    uint32_t dgrams;
    uint32_t dgrams_failed;  // the link would not take them
    uint64_t ts_bytes;
    uint32_t ppa_us_max, enc_us_max, mux_us_max, aud_us_max;
} se_stream_stats_t;

// Start streaming. `fb` is the framebuffer the game draws into; its
// size, format and orientation are read from it, and it is not kept.
//
// Everything that can fail and be REPORTED happens before the link goes
// up, while there is still a console to report it on: if this returns
// an error, the console is still there and nothing has been taken away.
esp_err_t se_stream_start(se_stream_cfg_t const* cfg, pax_buf_t const* fb);

// Take the H.264 encoder's memory early and hold it for the whole run,
// without starting a stream. Call it once, at boot, before the game has
// filled internal SRAM with anything of its own -- and know that it costs
// 57.6 KiB of internal SRAM for the rest of the run.
//
// WHY IT EXISTS. The encoder's reference frame is the one allocation on this
// badge that internal SRAM alone can satisfy: esp_h264_enc_hw_new() asks for
// 57 607 contiguous bytes with ESP_H264_MEM_INTERNAL and never tries PSRAM
// (esp_h264_enc_hw_param.c:382). Asked for at boot it lands. Asked for after
// a texture cache of 191 separate allocations has been laid over the same
// heap, it does not -- 133 KiB free and 38 KiB in one piece is enough in
// total and not enough in a row. Everything else the encoder owns, its 576
// KiB de-blocking buffer included, already falls back to PSRAM.
//
// OPTIONAL. Without it nothing changes: se_stream_start() builds the encoder
// on demand and se_stream_stop() deletes it again, buffers and all, exactly
// as it did before this call existed. Calling it early is what makes the
// difference between a livestream that starts and one that quietly never
// does; calling it is also the only way the memory is NOT handed back when a
// stream stops.
//
// Returns ESP_FAIL when the encoder would not allocate -- and there is a
// console to say so on, which there is not once a stream is up.
esp_err_t se_stream_reserve_permanent_memory(void);

// Stop, give the USB-C port back to the console, and free everything -- the
// encoder included, unless se_stream_reserve_permanent_memory() was called,
// in which case its buffers are kept for the next stream.
void se_stream_stop(void);

bool se_stream_running(void);

// Offer the finished frame. A no-op when not running, so the frame path
// need not ask first.
//
// CALL IT WITH THE FRAME STILL YOURS -- from the render callback, before
// the engine presents and moves on to the next page. The conversion
// reads the framebuffer, and a frame handed over any later is a frame
// being drawn into while it is read.
void se_stream_frame(pax_buf_t* fb);

void se_stream_get_stats(se_stream_stats_t* out);
