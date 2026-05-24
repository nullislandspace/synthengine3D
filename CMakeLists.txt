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
    src/nbt.c              # E3: NBT serialization primitive
    src/se_save.c          # E3: file-backed save-slot framework
    src/se_scene.c         # E4: z-buffer rasterizer + camera + projection
    src/se_run.c           # EF: application framework / run loop
    src/se_hw.c            # EF: device-global hardware settings (volume/brightness)
    src/se_ui.c            # EF: UI / list-menu system
    src/se_bindings.c      # EF: input bindings (remap + persistence)
    src/se_ppa.c           # E7: ESP32-P4 PPA blit helper (backdrop/sprite offload)
)

if(COMMAND idf_component_register)
    # ---- ESP-IDF component mode -------------------------------------
    idf_component_register(
        SRCS ${SE_SRCS}
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "src" "src/internal"
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
    add_library(synthengine3d OBJECT ${SE_SRCS})

    target_include_directories(synthengine3d
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
                ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/include/esp_additions/include
    )

    target_compile_options(synthengine3d PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -Os -fPIC
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
