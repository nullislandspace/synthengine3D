// =====================================================================
//  SynthEngine3D  --  application framework / run loop  (EF)
// ---------------------------------------------------------------------
//  Implements the inversion-of-control entry point declared in
//  include/se_run.h. The engine owns the device bootstrap (NVS, BSP,
//  display, double-buffered framebuffers, the 3D scene buffers, the
//  audio mixer, the vsync/tearing-effect semaphore), the per-frame loop
//  (delta-time, callback dispatch, default backdrop clear, blit at
//  vsync, buffer swap), the input-queue pump and the device-global keys
//  (volume +/-, audio-jack re-route, F1-exit when cfg.f1_exits), and the
//  blocking se_ui_capture_key rebind modal (it needs the loop's frame
//  primitives, so it lives here though declared in se_ui.h). Events the
//  pump does not consume are forwarded to the game's on_input callback.
//  The game plugs in via se_app_callbacks_t. See
//  ../devdocs/engine-extraction.md (EF) for the migration history.
// =====================================================================

#include "se_run.h"

#include "se_config.h"      // SE_FRAME_DT_MAX, SE_HW_VOLUME_STEP_PCT, SE_UI_*
#include "se_audio.h"       // audio_mixer_init, audio_mixer_shutdown
#include "se_hw.h"          // se_hw_init, se_hw_step_volume, se_hw_on_jack_event
#include "se_scene.h"       // scene_init
#include "se_ui.h"          // se_ui_capture_key (defined here -- needs the loop)
#include "se_text.h"        // rendertext_draw (capture prompt)
#include "se_direct565.h"   // direct_565_dim_rect (capture prompt panel)

#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"   // event/key enums + types
#include "gl_input.h"    // gl_input_get_queue (USB + native merged)
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "pax_gfx.h"

// ESP32-P4 PSRAM L2 cache line. The framebuffers live in PSRAM and are
// read by DMA (LCD scan-out) and by hardware blocks (e.g. a game's PPA
// backdrop), so they are allocated cache-line aligned and rounded up to
// a whole line so their tail does not share a line with neighbours.
#define SE_PSRAM_CACHE_LINE 128

static char const TAG[] = "se_run";

// ---- Engine-owned display + framebuffer state -----------------------
static se_display_info_t  s_di         = {0};
static pax_buf_t          s_fb_a       = {0};
static pax_buf_t          s_fb_b       = {0};
static pax_buf_t*         s_fb         = &s_fb_a;   // back buffer (drawn into)
static pax_buf_t*         s_fb_front   = &s_fb_b;   // front buffer (scanned out)
static void*              s_fb_a_px    = NULL;
static void*              s_fb_b_px    = NULL;
static SemaphoreHandle_t  s_vsync_sem  = NULL;

// Loop control. Set by se_request_exit(); the loop checks it once per
// frame and falls out (firing on_shutdown) when set.
static volatile bool      s_exit_requested = false;

// ---- Input pump state -----------------------------------------------
static QueueHandle_t      s_input_queue    = NULL;   // BSP event queue
static bool               s_f1_exits       = false;  // from se_app_config_t

// Callbacks + config retained for blocking modals (se_ui_capture_key)
// that need to re-run the backdrop hook + present frames mid-callback.
static se_app_callbacks_t const* s_cb            = NULL;
static void*                     s_user          = NULL;
static uint32_t                  s_backdrop_argb = 0;

void se_request_exit(void) {
    s_exit_requested = true;
}

void se_display_info(se_display_info_t* out) {
    if (out) *out = s_di;
}

// Draw the current frame's backdrop: the game's hook, or a flat clear.
static void se_draw_backdrop(void) {
    if (s_cb && s_cb->on_backdrop) {
        s_cb->on_backdrop(s_fb, s_user);
    } else {
        pax_background(s_fb, s_backdrop_argb);
    }
}

// Drain the BSP input queue once. The engine consumes the device-global
// keys live -- volume +/- (active output, via se_hw), the audio-jack
// re-route, and F1-exit when the game opted in -- and forwards every
// other event to on_input. The power button and F2/F3 are deliberately
// left untouched (the power button's 2 s-hold power-off lives in the
// coprocessor; function keys are too valuable to spend).
static void se_pump_input(se_app_callbacks_t const* cb, void* user) {
    if (s_input_queue == NULL) return;

    bsp_input_event_t ev;
    while (xQueueReceive(s_input_queue, &ev, 0) == pdTRUE) {
        // Audio-jack is pure device routing -- the engine consumes it.
        if (ev.type == INPUT_EVENT_TYPE_ACTION &&
            ev.args_action.type == BSP_INPUT_ACTION_TYPE_AUDIO_JACK) {
            se_hw_on_jack_event(ev.args_action.state);
            continue;
        }
        // Volume +/- and F1-exit are consumed live.
        if (ev.type == INPUT_EVENT_TYPE_NAVIGATION && ev.args_navigation.state) {
            bsp_input_navigation_key_t const key = ev.args_navigation.key;
            if (key == BSP_INPUT_NAVIGATION_KEY_VOLUME_UP) {
                se_hw_step_volume(+SE_HW_VOLUME_STEP_PCT);
                continue;
            }
            if (key == BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN) {
                se_hw_step_volume(-SE_HW_VOLUME_STEP_PCT);
                continue;
            }
            if (key == BSP_INPUT_NAVIGATION_KEY_F1 && s_f1_exits) {
                // Return to launcher (audio down first). Does not return
                // under graceloader.
                audio_mixer_shutdown();
                bsp_device_restart_to_launcher();
                continue;
            }
        }
        // Not an engine-consumed event -- hand it to the game.
        if (cb->on_input) {
            cb->on_input(&ev, user);
        }
    }
}

// Present the back buffer: hand it to the LCD, wait for the vsync
// (tearing-effect) signal, then swap so the buffer just sent becomes the
// front (scanned out) and the previous front becomes the next back. The
// CPU and any hardware block only ever touch *s_fb, never the buffer the
// LCD is reading -- no tearing.
static void se_present(void) {
    bsp_display_blit(0, 0, s_di.width, s_di.height, pax_buf_get_pixels(s_fb));

    if (s_vsync_sem != NULL) {
        xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(50));
    } else {
        vTaskDelay(pdMS_TO_TICKS(16));
    }

    pax_buf_t* tmp = s_fb;
    s_fb           = s_fb_front;
    s_fb_front     = tmp;
}

// ---- Rebind key capture ---------------------------------------------

// Map one input event to the BSP scancode it would bind, or 0 if it is
// not a single bindable key press. Function keys arrive on the navigation
// channel but have scancode equivalents; plain keys arrive as single-byte
// scancode presses (release events have the high bit set; escaped
// multi-byte scancodes >= 0xE000 don't make sensible single-key binds).
static uint16_t se_bindable_scancode(bsp_input_event_t const* ev) {
    if (ev->type == INPUT_EVENT_TYPE_NAVIGATION && ev->args_navigation.state) {
        switch (ev->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_F1:  return BSP_INPUT_SCANCODE_F1;
            case BSP_INPUT_NAVIGATION_KEY_F2:  return BSP_INPUT_SCANCODE_F2;
            case BSP_INPUT_NAVIGATION_KEY_F3:  return BSP_INPUT_SCANCODE_F3;
            case BSP_INPUT_NAVIGATION_KEY_F4:  return BSP_INPUT_SCANCODE_F4;
            case BSP_INPUT_NAVIGATION_KEY_F5:  return BSP_INPUT_SCANCODE_F5;
            case BSP_INPUT_NAVIGATION_KEY_F6:  return BSP_INPUT_SCANCODE_F6;
            case BSP_INPUT_NAVIGATION_KEY_F7:  return BSP_INPUT_SCANCODE_F7;
            case BSP_INPUT_NAVIGATION_KEY_F8:  return BSP_INPUT_SCANCODE_F8;
            case BSP_INPUT_NAVIGATION_KEY_F9:  return BSP_INPUT_SCANCODE_F9;
            case BSP_INPUT_NAVIGATION_KEY_F10: return BSP_INPUT_SCANCODE_F10;
            case BSP_INPUT_NAVIGATION_KEY_F11: return BSP_INPUT_SCANCODE_F11;
            case BSP_INPUT_NAVIGATION_KEY_F12: return BSP_INPUT_SCANCODE_F12;
            default: return 0;
        }
    }
    if (ev->type == INPUT_EVENT_TYPE_SCANCODE) {
        uint16_t const sc = ev->args_scancode.scancode;
        if (sc != BSP_INPUT_SCANCODE_NONE
            && (sc & BSP_INPUT_SCANCODE_RELEASE_MODIFIER) == 0
            && sc < 0xE000u) {
            return sc;
        }
    }
    return 0;
}

// Draw the "press a key" modal: a dim panel + heading + the control label
// + a hint. Mirrors the layout the game used before the dialog moved into
// the engine.
static void se_draw_capture_prompt(char const* label) {
    float const fbw = pax_buf_get_widthf(s_fb);
    float const fbh = pax_buf_get_heightf(s_fb);
    int   const pw  = (int)(fbw * 0.56f);
    int   const ph  = (int)(fbh * 0.44f);
    int   const px  = (int)((fbw - (float)pw) * 0.5f);
    int   const py  = (int)((fbh - (float)ph) * 0.5f);
    direct_565_dim_rect((uint16_t*)pax_buf_get_pixels(s_fb), s_fb->reverse_endianness,
                        px, py, pw, ph);

    float const lx = (float)px + SE_UI_TEXT_INSET;
    rendertext_draw(s_fb, SE_UI_COL_TITLE, NULL, 36.0f, lx, fbh * 0.40f, "Press a key");
    if (label) {
        char prompt[48];
        snprintf(prompt, sizeof(prompt), "Rebinding: %s", label);
        rendertext_draw(s_fb, SE_UI_COL_NORMAL, NULL, 22.0f, lx, fbh * 0.56f, prompt);
    }
    rendertext_draw(s_fb, SE_UI_COL_HINT, NULL, 14.0f, lx, fbh * 0.66f,
                    "the next key you press becomes the binding");
}

uint16_t se_ui_capture_key(char const* prompt_label) {
    if (s_input_queue == NULL) return 0;

    uint16_t captured = 0;
    while (captured == 0 && !s_exit_requested) {
        // Drain the queue ourselves (bypassing the normal pump), grabbing
        // the first bindable key -- so volume / F1, which the pump would
        // otherwise consume, can be bound here too.
        bsp_input_event_t ev;
        while (xQueueReceive(s_input_queue, &ev, 0) == pdTRUE) {
            if (captured == 0) {
                uint16_t const sc = se_bindable_scancode(&ev);
                if (sc != 0) captured = sc;
            }
        }
        se_draw_backdrop();
        se_draw_capture_prompt(prompt_label);
        se_present();
    }
    return captured;
}

// Bootstrap NVS, BSP, the display, the framebuffers, the scene buffers,
// the audio mixer and vsync. Returns true on success; on failure logs
// and returns false (se_run then bails before the loop).
static bool se_bootstrap(void) {
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    bsp_configuration_t const bsp_configuration = {
        .display =
            {
                .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_16_565RGB,
                .num_fbs                = 1,
            },
    };
    res = bsp_device_initialize(&bsp_configuration);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BSP: %d", res);
        return false;
    }

    size_t                        h_res = 0, v_res = 0;
    bsp_display_color_format_t    color_format = BSP_DISPLAY_COLOR_FORMAT_24_888RGB;
    bsp_display_endianness_t      data_endian  = BSP_DISPLAY_ENDIAN_LITTLE;
    res = bsp_display_get_parameters(&h_res, &v_res, &color_format, &data_endian);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get display parameters: %d", res);
        return false;
    }

    pax_buf_type_t format = PAX_BUF_24_888RGB;
    switch (color_format) {
        case BSP_DISPLAY_COLOR_FORMAT_16_565RGB: format = PAX_BUF_16_565RGB; break;
        case BSP_DISPLAY_COLOR_FORMAT_24_888RGB: format = PAX_BUF_24_888RGB; break;
        default: break;
    }

    pax_orientation_t orientation = PAX_O_UPRIGHT;
    switch (bsp_display_get_default_rotation()) {
        case BSP_DISPLAY_ROTATION_90:  orientation = PAX_O_ROT_CCW;  break;
        case BSP_DISPLAY_ROTATION_180: orientation = PAX_O_ROT_HALF; break;
        case BSP_DISPLAY_ROTATION_270: orientation = PAX_O_ROT_CW;   break;
        case BSP_DISPLAY_ROTATION_0:
        default:                       orientation = PAX_O_UPRIGHT;  break;
    }

    s_di.width       = h_res;
    s_di.height      = v_res;
    s_di.pax_format  = format;
    s_di.reversed    = (data_endian == BSP_DISPLAY_ENDIAN_BIG);
    s_di.orientation = orientation;

    // Two PSRAM framebuffers, PPA/DMA-aligned, each wrapped in a pax_buf_t
    // so PAX rasterises straight into the raw layout the LCD reads.
    size_t const fb_size         = (size_t)h_res * v_res * 2u;
    size_t const aligned_fb_size =
        (fb_size + SE_PSRAM_CACHE_LINE - 1) & ~(size_t)(SE_PSRAM_CACHE_LINE - 1);
    s_fb_a_px = heap_caps_aligned_alloc(SE_PSRAM_CACHE_LINE, aligned_fb_size, MALLOC_CAP_SPIRAM);
    s_fb_b_px = heap_caps_aligned_alloc(SE_PSRAM_CACHE_LINE, aligned_fb_size, MALLOC_CAP_SPIRAM);
    if (s_fb_a_px == NULL || s_fb_b_px == NULL) {
        ESP_LOGE(TAG, "Failed to allocate framebuffers (a=%p b=%p)", s_fb_a_px, s_fb_b_px);
        return false;
    }

    pax_buf_init(&s_fb_a, s_fb_a_px, h_res, v_res, format);
    pax_buf_reversed(&s_fb_a, s_di.reversed);
    pax_buf_set_orientation(&s_fb_a, orientation);

    pax_buf_init(&s_fb_b, s_fb_b_px, h_res, v_res, format);
    pax_buf_reversed(&s_fb_b, s_di.reversed);
    pax_buf_set_orientation(&s_fb_b, orientation);

    s_fb       = &s_fb_a;
    s_fb_front = &s_fb_b;

    // 3D scene buffers (depth/stamp/line list) -- compile-time sized.
    scene_init();

    // Software audio mixer + I2S. Non-fatal: a failure just means silence.
    res = audio_mixer_init();
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "audio_mixer_init failed: %d -- audio will be silent", res);
    }

    // Device-global hardware settings (volume / brightness, jack routing).
    // After audio_mixer_init so its raw-jack amplifier default is overlaid
    // by the launcher-persisted volume + routing here.
    se_hw_init();

    // Input event queue (drained each frame by se_pump_input).
    // gl_input_get_queue returns the same queue as bsp_input_get_queue
    // but the graceloader also feeds it from any plugged-in USB keyboard.
    res = gl_input_get_queue(&s_input_queue);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "gl_input_get_queue failed: %d -- input will be dead", res);
        s_input_queue = NULL;
    }

    // Vsync via the panel tearing-effect line. Optional: without it the
    // loop falls back to a fixed delay (animation may stutter).
    esp_err_t te_err = bsp_display_set_tearing_effect_mode(BSP_DISPLAY_TE_V_BLANKING);
    if (te_err == ESP_OK) {
        te_err = bsp_display_get_tearing_effect_semaphore(&s_vsync_sem);
    }
    if (te_err != ESP_OK || s_vsync_sem == NULL) {
        ESP_LOGW(TAG, "Vsync not available -- animation may stutter");
        s_vsync_sem = NULL;
    }

    return true;
}

void se_run(se_app_config_t const* cfg, se_app_callbacks_t const* cb, void* user) {
    se_app_config_t lcfg = {0};
    if (cfg) lcfg = *cfg;

    s_exit_requested = false;
    s_f1_exits       = lcfg.f1_exits;
    s_backdrop_argb  = lcfg.backdrop_argb;

    if (cb == NULL || cb->on_update == NULL) {
        ESP_LOGE(TAG, "se_run: callbacks (and on_update) are required");
        return;
    }
    s_cb   = cb;     // retained for blocking modals (se_ui_capture_key)
    s_user = user;

    if (!se_bootstrap()) {
        return;
    }

    // Game-side init: world, save, content load, the game's own bootstrap
    // (backdrop caches, input, settings). Runs after the engine has the
    // display + audio + scene up, so se_display_info() is already valid.
    if (cb->on_init) {
        cb->on_init(user);
    }

    int64_t prev_us = esp_timer_get_time();
    while (!s_exit_requested) {
        int64_t const now_us = esp_timer_get_time();
        float         dt      = (float)(now_us - prev_us) / 1e6f;
        prev_us               = now_us;
        if (dt > SE_FRAME_DT_MAX) dt = SE_FRAME_DT_MAX;

        // Drain the input queue first: consume the device-global keys,
        // forward the rest to on_input so the game's per-frame consume
        // accessors see this frame's events in on_update below.
        se_pump_input(cb, user);

        // Per-frame game logic / state machine.
        cb->on_update(dt, user);

        // Backdrop: the game's hook, or a flat clear when none is set.
        se_draw_backdrop();

        // Foreground: 3D scene, HUD, menus.
        if (cb->on_render) {
            cb->on_render(s_fb, user);
        }

        // Hand the finished frame to the LCD at vsync, then swap buffers.
        se_present();
    }

    if (cb->on_shutdown) {
        cb->on_shutdown(user);
    }
}
