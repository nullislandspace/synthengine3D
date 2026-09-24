// =====================================================================
//  SynthEngine3D  --  application framework / run loop  (EF)
// ---------------------------------------------------------------------
//  Implements the inversion-of-control entry point declared in
//  include/se_run.h. The engine owns the device bootstrap (NVS, BSP,
//  display and its three page-flipped framebuffers, the 3D scene buffers,
//  the audio mixer, the refresh-done semaphore), the per-frame loop
//  (delta-time, callback dispatch, default backdrop clear, page flip),
//  the input-queue pump and the device-global keys
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
#include "se_frame.h"       // se_frame_back / se_frame_present (internal)
#include "se_scene.h"       // scene_init
#include "se_ui.h"          // se_ui_capture_key (defined here -- needs the loop)
#include "se_text.h"        // rendertext_draw (capture prompt)
#include "se_direct565.h"   // direct_565_dim_rect (capture prompt panel)

#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"   // event/key enums + types
#include "esp_cache.h"         // esp_cache_msync
#include "esp_lcd_mipi_dsi.h"   // esp_lcd_dpi_panel_get_frame_buffer
#include "esp_lcd_panel_ops.h"  // esp_lcd_panel_draw_bitmap
#include "gl_input.h"    // gl_input_get_queue (USB + native merged)
#include "graceloader.h" // graceloader_display_register_callbacks
#include "esp_log.h"
#include "esp_timer.h"
#include "hal/lcd_types.h"
#include "nvs_flash.h"
#include "pax_gfx.h"

static char const TAG[] = "se_run";

// ---- Engine-owned display + framebuffer state -----------------------
static se_display_info_t  s_di         = {0};
// The display driver's three framebuffers, triple-buffered (se_present).
// s_fb_shown is what the display read at its last refresh, s_fb_selected
// what the engine last handed it; the one that is neither is drawn into.
#define SE_FB_COUNT 3
static pax_buf_t              s_fbs[SE_FB_COUNT]  = {0};
static pax_buf_t*             s_fb                = NULL;   // back buffer (drawn into)
static int                    s_fb_back           = 1;
static volatile int           s_fb_shown          = 0;      // written by se_on_refresh_done
static volatile int           s_fb_selected       = 0;
static esp_lcd_panel_handle_t s_panel             = NULL;
static SemaphoreHandle_t      s_refresh_sem       = NULL;   // given once per display refresh

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

// Called by graceloader from the display's interrupt, once per refresh
// (graceloader_display_register_callbacks), right after the interrupt
// restarted the refresh with the buffer the driver has selected. So the
// display now reads s_fb_selected -- or, if a flip is between its
// draw_bitmap and the store to s_fb_selected, the buffer being flipped
// to, which se_present never draws into either.
//
// Skipped while a flash write has the cache off. The refresh still runs,
// so the display may move on to the selected buffer unrecorded; but the
// game's tasks cannot run then either, and se_present waits for a record
// before it flips again, so a skip only delays that wait by a refresh.
static bool se_on_refresh_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t* edata,
                               void* user_ctx) {
    (void)panel;
    (void)edata;
    (void)user_ctx;
    s_fb_shown       = s_fb_selected;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_refresh_sem, &woken);
    return woken == pdTRUE;
}

// Present the back buffer by page flipping between the display driver's
// own three framebuffers: draw_bitmap copies nothing, it writes the back
// buffer out of the cache and selects it for the next refresh.
//
// Triple buffering, so a frame never waits for the display to let go of
// a buffer: the display reads one (shown), one waits for the next
// refresh (selected), and the third is drawn into. The display only
// ever reads shown or selected, so the CPU and any hardware block, which
// only touch *s_fb, never draw into a buffer being read -- no tearing.
//
// The one wait is before the flip: a frame is only handed over once the
// display has picked up the previous one. A game faster than the refresh
// rate waits here instead of drawing frames that are never shown; a
// slower one finds it done and does not wait at all. The semaphore is
// cleared before s_fb_shown is checked, and the callback stores
// s_fb_shown before it gives, so a refresh in between cannot be missed.
//
// This relies on the callback coming from the interrupt that restarts
// the refresh, which holds for ESP32-P4 builds below chip revision 3.0
// (graceloader's). From 3.0 on it comes from the DSI bridge's vsync,
// slightly later, and a flip in between would be recorded as shown
// while the display reads the previous buffer (see graceloader.h).
//
// Both halves are timed (se_present_stats): the wait is idle, the flip
// is work (the cache write-back), and a game reading one lump "present"
// figure cannot tell them apart.
static int64_t s_present_blit_us  = 0;
static int64_t s_present_vsync_us = 0;

static void se_present(void) {
    int64_t const t0 = esp_timer_get_time();
    xSemaphoreTake(s_refresh_sem, 0);
    while (s_fb_shown != s_fb_selected) {
        if (xSemaphoreTake(s_refresh_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
            break;   // the display is not refreshing; nothing reads the buffers
        }
    }
    // Shown and selected are the same buffer now; read it once; the
    // callback may overwrite s_fb_shown at any point below.
    int const shown = s_fb_selected;
    int64_t const t1 = esp_timer_get_time();

    // Write the frame back AND drop it from the cache (draw_bitmap only
    // writes back). The buffer is drawn into again two frames on, after
    // the PPA may have filled it by DMA behind the cache: a line still
    // cached from this frame would then hold old pixels, and a partial
    // CPU write into it (a HUD glyph) would write them back over the new
    // backdrop. This used to be left to the frame's working set evicting
    // everything; with depth in internal SRAM (SE_RENDER_BANDED) it may
    // not. draw_bitmap's own write-back then finds nothing to do.
    void* const px = pax_buf_get_pixels_rw(s_fb);
    esp_cache_msync(px, pax_buf_get_size(s_fb), ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, (int)s_di.width, (int)s_di.height, px);
    s_fb_selected = s_fb_back;
    // The display reads `shown` or the buffer just selected; the next back
    // buffer is the third one.
    s_fb_back = 3 - shown - s_fb_selected;
    s_fb      = &s_fbs[s_fb_back];
    int64_t const t2 = esp_timer_get_time();

    s_present_vsync_us = t1 - t0;
    s_present_blit_us  = t2 - t1;
}

// ---- Internal frame access (src/internal/se_frame.h) -----------------
//
// Lets an engine facility outside this file run its own present loop (the
// splash screen). Gated on s_fb so a call made before se_run() has
// bootstrapped reports "no frame" instead of handing out a zero-initialised
// pax_buf_t.

void se_present_stats(int64_t* blit_us, int64_t* vsync_us) {
    if (blit_us != NULL) *blit_us = s_present_blit_us;
    if (vsync_us != NULL) *vsync_us = s_present_vsync_us;
}

pax_buf_t* se_frame_back(void) {
    return s_fb;
}

void se_frame_present(void) {
    if (s_fb != NULL) se_present();
}

// ---- Rebind key capture ---------------------------------------------

// Map one input event to the BSP scancode it would bind, or 0 if it is
// not a single bindable key press.
//
// Plain keys arrive as single-byte scancode presses (a release has
// BSP_INPUT_SCANCODE_RELEASE_MODIFIER set). The cursor keys and the
// grey block (Home, End, Page Up/Down, Insert, Delete) are ESCAPED
// scancodes, 0xE0xx, and they bind like any other: a binding is a
// uint16_t and gl_input_read_scancode polls an escaped code as happily
// as a plain one. They were refused until 2.1, which made the arrow keys
// -- a natural default for looking or steering -- impossible to bind
// back once a player had changed them. Only the "fake shift" codes some
// keyboards wrap round the grey keys are refused, since they are not a
// key anyone pressed.
//
// Some keyboards send the cursor keys and the function keys only on the
// navigation channel, so those map onto the scancode the other keyboards
// send: the same physical key must bind to the same value whichever
// keyboard pressed it.
static uint16_t se_bindable_scancode(bsp_input_event_t const* ev) {
    if (ev->type == INPUT_EVENT_TYPE_NAVIGATION && ev->args_navigation.state) {
        switch (ev->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_F1:    return BSP_INPUT_SCANCODE_F1;
            case BSP_INPUT_NAVIGATION_KEY_F2:    return BSP_INPUT_SCANCODE_F2;
            case BSP_INPUT_NAVIGATION_KEY_F3:    return BSP_INPUT_SCANCODE_F3;
            case BSP_INPUT_NAVIGATION_KEY_F4:    return BSP_INPUT_SCANCODE_F4;
            case BSP_INPUT_NAVIGATION_KEY_F5:    return BSP_INPUT_SCANCODE_F5;
            case BSP_INPUT_NAVIGATION_KEY_F6:    return BSP_INPUT_SCANCODE_F6;
            case BSP_INPUT_NAVIGATION_KEY_F7:    return BSP_INPUT_SCANCODE_F7;
            case BSP_INPUT_NAVIGATION_KEY_F8:    return BSP_INPUT_SCANCODE_F8;
            case BSP_INPUT_NAVIGATION_KEY_F9:    return BSP_INPUT_SCANCODE_F9;
            case BSP_INPUT_NAVIGATION_KEY_F10:   return BSP_INPUT_SCANCODE_F10;
            case BSP_INPUT_NAVIGATION_KEY_F11:   return BSP_INPUT_SCANCODE_F11;
            case BSP_INPUT_NAVIGATION_KEY_F12:   return BSP_INPUT_SCANCODE_F12;
            case BSP_INPUT_NAVIGATION_KEY_UP:    return BSP_INPUT_SCANCODE_ESCAPED_GREY_UP;
            case BSP_INPUT_NAVIGATION_KEY_DOWN:  return BSP_INPUT_SCANCODE_ESCAPED_GREY_DOWN;
            case BSP_INPUT_NAVIGATION_KEY_LEFT:  return BSP_INPUT_SCANCODE_ESCAPED_GREY_LEFT;
            case BSP_INPUT_NAVIGATION_KEY_RIGHT: return BSP_INPUT_SCANCODE_ESCAPED_GREY_RIGHT;
            case BSP_INPUT_NAVIGATION_KEY_HOME:  return BSP_INPUT_SCANCODE_ESCAPED_GREY_HOME;
            case BSP_INPUT_NAVIGATION_KEY_END:   return BSP_INPUT_SCANCODE_ESCAPED_GREY_END;
            case BSP_INPUT_NAVIGATION_KEY_PGUP:  return BSP_INPUT_SCANCODE_ESCAPED_GREY_PGUP;
            case BSP_INPUT_NAVIGATION_KEY_PGDN:  return BSP_INPUT_SCANCODE_ESCAPED_GREY_PGDN;
            default: return 0;
        }
    }
    if (ev->type == INPUT_EVENT_TYPE_SCANCODE) {
        uint16_t const sc = ev->args_scancode.scancode;
        if (sc == BSP_INPUT_SCANCODE_NONE || (sc & BSP_INPUT_SCANCODE_RELEASE_MODIFIER) != 0) return 0;
        if (sc == BSP_INPUT_SCANCODE_ESCAPED_FAKE_LSHIFT || sc == BSP_INPUT_SCANCODE_ESCAPED_FAKE_RSHIFT) return 0;
        if (sc < 0xE000u || (sc & 0xFF00u) == 0xE000u) return sc;
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

// Bootstrap NVS, BSP, the display, the framebuffers and the refresh
// callback, the scene buffers and the audio mixer. Returns true on success; on failure logs
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
                .num_fbs                = SE_FB_COUNT,   // page flipping, see se_present
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

    // The display driver's framebuffers (PSRAM, cache-line aligned by the
    // driver), each wrapped in a pax_buf_t so PAX rasterises straight into
    // the raw layout the display reads. The driver shows the first one
    // after initialisation, so drawing starts in the second.
    void* fb_px[SE_FB_COUNT] = {NULL};
    res = bsp_display_get_panel(&s_panel);
    if (res == ESP_OK) {
        res = esp_lcd_dpi_panel_get_frame_buffer(s_panel, SE_FB_COUNT, &fb_px[0], &fb_px[1], &fb_px[2]);
    }
    if (res != ESP_OK || fb_px[0] == NULL || fb_px[1] == NULL || fb_px[2] == NULL) {
        ESP_LOGE(TAG, "Failed to get the display's framebuffers: %s", esp_err_to_name(res));
        return false;
    }
    for (int i = 0; i < SE_FB_COUNT; i++) {
        pax_buf_init(&s_fbs[i], fb_px[i], h_res, v_res, format);
        pax_buf_reversed(&s_fbs[i], s_di.reversed);
        pax_buf_set_orientation(&s_fbs[i], orientation);
    }
    s_fb_shown    = 0;
    s_fb_selected = 0;
    s_fb_back     = 1;

    // Refresh-done, for the page flip. graceloader forwards the driver's
    // interrupt (it only accepts IRAM callbacks, and the game is in PSRAM).
    s_refresh_sem = xSemaphoreCreateBinary();
    if (s_refresh_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create the refresh semaphore");
        return false;
    }
    esp_lcd_dpi_panel_event_callbacks_t const display_cbs = {.on_refresh_done = se_on_refresh_done};
    res = graceloader_display_register_callbacks(&display_cbs, NULL);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register the display refresh callback: %s", esp_err_to_name(res));
        return false;
    }

    s_fb = &s_fbs[s_fb_back];   // last: se_frame_back() reports "ready" from here on

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

        // Flip to the finished frame at the next refresh.
        se_present();
    }

    if (cb->on_shutdown) {
        cb->on_shutdown(user);
    }
}
