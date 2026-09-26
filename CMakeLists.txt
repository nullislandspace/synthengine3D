# SynthEngine3D -- reusable engine component for Tanmatsu graceloader apps.
#
# DUAL-MODE build file:
#   * Under the ESP-IDF build system (idf.py / graceloader): registers a
#     real IDF component via idf_component_register(), so a full idf.py app
#     (e.g. graceloader) can consume it -- vendored or externally managed.
#   * Under THIS game's plain-CMake build (no idf.py): defines a relocatable
#     OBJECT library whose objects are folded into the app's app.so by the
#     top-level hand-rolled link script.
#
# Public, STABLE API lives in include/ (the only consumer-visible dir).
# Internal headers live in src/internal/ and are never on a consumer's path.
# See ../devdocs/engine-extraction.md for the plan and the performance
# contract (identical compile flags => identical codegen).

set(SE_SRCS
    src/se_version.c
    src/rendertext.c       # E1: Hershey vector text (wraps src/internal/hershey*)
    src/audio_dsp.c        # E2: DSP primitives (oscillators, env, biquad)
    src/audio_mixer.c      # E2: software mixer + I2S, group-gated output
    src/se_voice.c         # E2: pluggable synth voices (built-in subtractive/noise)
    src/music_procedural.c # E2: procedural music source
    src/se_mp3.c           # E2: MP3 music source (minimp3, vendored)
    src/nbt.c              # E3: NBT serialization primitive
    src/se_save.c          # E3: file-backed save-slot framework
    src/se_scene.c         # E4: z-buffer rasterizer + camera + projection
    src/se_light.c         # E4: scene lighting (one positional light)
    src/se_texture.c       # E4: PNG textures for scene_textured_tri
    src/se_run.c           # EF: application framework / run loop
    src/se_splash.c        # EF: 3D engine splash screen
    src/se_hw.c            # EF: device-global hardware settings (volume/brightness)
    src/se_ui.c            # EF: UI / list-menu system
    src/se_bindings.c      # EF: input bindings (remap + persistence)
    src/se_ppa.c           # E7: ESP32-P4 PPA blit helper (backdrop/sprite offload)
    src/se_stream.c        # E8: live A/V streaming, MPEG-TS over UDP
    src/internal/tsmux.c   # E8: MPEG-TS muxer (plain C, host-testable)
    src/internal/netraw.c  # E8: ARP/ICMP/DHCP/UDP for the link
    src/internal/usbnet.c  # E8: the USB-C port as a CDC-NCM adapter
    src/internal/se_stream_audio.c  # E8: the mixer's output into the stream
    # pdmp2: the MPEG Layer II encoder behind se_stream's cfg.audio.
    # A SUBMODULE, not a vendored copy -- github.com/nullislandspace/
    # public-domain-mp2-encoder -- so its tests and the tool that measures
    # its filterbank window come with it, and a fix goes upstream rather
    # than into a copy. Its PROVENANCE.md says where every number in it
    # came from. PUBLIC DOMAIN, which is the whole reason it was written:
    # shine, twolame and LAME are all LGPL, and an app.so that ships as
    # one blob nobody can relink is the worst case for LGPL section 6.
    # Nothing in this engine is copyleft.
    src/internal/pdmp2/src/pdmp2.c
    src/internal/pdmp2/src/pdmp2_tables.c
    src/internal/pdmp2/src/pdmp2_window.c
)

# A submodule that was never checked out fails as a hundred missing-header
# errors deep in a build log. Say it once, at the top, in words.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/pdmp2/src/pdmp2.c")
    message(FATAL_ERROR
        "src/internal/pdmp2 is empty: the pdmp2 submodule is not checked out.\n"
        "Run:  git submodule update --init --recursive")
endif()

# TinyUSB, vendored (src/internal/tinyusb/PROVENANCE.md). ONLY IN APP
# MODE: under the IDF the host (graceloader, the launcher) already has a
# USB stack and two of them in one binary is one too many. An app built
# by the plain-CMake path has none, so it brings this one.
set(SE_TINYUSB_SRCS
    src/internal/tinyusb/src/tusb.c
    src/internal/tinyusb/src/common/tusb_fifo.c
    src/internal/tinyusb/src/device/usbd.c
    src/internal/tinyusb/src/class/net/ncm_device.c
    src/internal/tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c
    src/internal/tinyusb/src/portable/synopsys/dwc2/dwc2_common.c
)

if(COMMAND idf_component_register)
    # ---- ESP-IDF component mode -------------------------------------
    idf_component_register(
        SRCS ${SE_SRCS}
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "src" "src/internal" "src/internal/tinyusb/src"
                          "src/internal/pdmp2/include" "src/internal/pdmp2/src"
        # esp_driver_ppa: the PPA peripheral driver (se_ppa.c);
        # esp_mm: esp_cache_msync for the PPA layer-cache flush.
        REQUIRES freertos esp_driver_ppa esp_mm
    )
else()
    # ---- Plain-CMake mode (this game's app.so build) ----------------
    # Mirror app_obj's compile environment exactly. The xesppie march /
    # ilp32f abi that carry the PIE SIMD come from toolchain-app.cmake's
    # CMAKE_C_FLAGS_INIT and apply to every target in the project, so they
    # are inherited here automatically; the per-target options below match
    # the top-level CMakeLists.txt verbatim.
    # TinyUSB only here: see where SE_TINYUSB_SRCS is defined.
    add_library(synthengine3d OBJECT ${SE_SRCS} ${SE_TINYUSB_SRCS})
    # PDMP2_CONFIG_H puts the encoder's ~35 KB working set in PSRAM
    # (src/internal/pdmp2_port.h); internal SRAM is the scarce thing here.
    target_compile_definitions(synthengine3d PRIVATE
        SE_STREAM_AUDIO_CODEC=1
        PDMP2_CONFIG_H="pdmp2_port.h")
    # Upstream code, kept byte-identical: its warnings are not ours.
    set_source_files_properties(${SE_TINYUSB_SRCS} PROPERTIES COMPILE_OPTIONS "-w")

    target_include_directories(synthengine3d
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/tinyusb/src
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/pdmp2/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal/pdmp2/src
                ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/include/esp_additions/include
    )

    target_compile_options(synthengine3d PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -O2 -fPIC
        -ffunction-sections -fdata-sections -fno-common
        -D__thread=
        -DESP_PLATFORM
    )

    # ESP-IDF normally force-includes FreeRTOS headers for all components.
    target_compile_options(synthengine3d PRIVATE
        "SHELL:-include freertos/FreeRTOS.h"
        "SHELL:-include freertos/semphr.h"
        "SHELL:-include freertos/task.h"
        "SHELL:-include freertos/queue.h"
        "SHELL:-include freertos/event_groups.h"
    )
endif()
