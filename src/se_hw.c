// =====================================================================
//  SynthEngine3D  --  device-global hardware settings (see se_hw.h)
// ---------------------------------------------------------------------
//  Volume / brightness are global codec / peripheral registers; we set
//  them once at boot (se_hw_init) per the launcher's persisted values,
//  then poke them again only on user input (volume keys or audio-jack
//  hot-swap, both driven by se_run's input pump). No per-frame work.
// =====================================================================

#include "se_hw.h"

#include "bsp/audio.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "gl_input.h"
#include "esp_log.h"
#include "nvs_settings_hardware.h"
#include "se_config.h"   // SE_HW_DISPLAY_BRIGHTNESS_MIN

static char const TAG[] = "se_hw";

#define VOLUME_DEFAULT_PERCENT      50
#define BACKLIGHT_DEFAULT_PERCENT   50
#define KEYBOARD_BL_DEFAULT_PERCENT 50
#define LED_BRIGHTNESS_DEFAULT_PCT  50

static bool s_jack_inserted = false;

static uint8_t clamp_pct_int(int v) {
    if (v < 0)   return 0;
    if (v > 100) return 100;
    return (uint8_t)v;
}

static uint8_t clamp_pct_min(uint8_t v, uint8_t lo) {
    if (v < lo)  return lo;
    if (v > 100) return 100;
    return v;
}

// Read the active output's persisted volume from the launcher's NVS
// namespace. Defaults to VOLUME_DEFAULT_PERCENT if the key isn't there
// yet (fresh device or first-boot launcher).
static uint8_t read_active_volume(void) {
    uint8_t v = VOLUME_DEFAULT_PERCENT;
    if (s_jack_inserted) {
        nvs_settings_get_headphone_volume(&v, VOLUME_DEFAULT_PERCENT);
    } else {
        nvs_settings_get_speaker_volume(&v, VOLUME_DEFAULT_PERCENT);
    }
    return v;
}

// Apply the currently-cached jack state: pick the right NVS value, push
// to the codec, mute the speaker amp when headphones are in.
static void apply_audio_routing(void) {
    uint8_t const v = read_active_volume();
    esp_err_t err = bsp_audio_set_volume((float)v);
    ESP_LOGI(TAG, "apply_audio_routing: jack=%s vol=%u set_volume_err=%d",
             s_jack_inserted ? "in" : "out", (unsigned)v, err);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bsp_audio_set_volume(%u) failed: %d", (unsigned)v, err);
    }
    // Speaker amplifier off when headphones are inserted -- the codec
    // drives both lines but the speaker amp is a separate chip we don't
    // want driving residual signal into the speaker.
    err = bsp_audio_set_amplifier(!s_jack_inserted);
    ESP_LOGI(TAG, "apply_audio_routing: amp=%s set_amp_err=%d",
             s_jack_inserted ? "off" : "on", err);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bsp_audio_set_amplifier(%d) failed: %d", !s_jack_inserted, err);
    }
}

esp_err_t se_hw_init(void) {
    ESP_LOGI(TAG, "se_hw_init: begin");

    // ---- Brightnesses ----
    uint8_t pct;

    if (nvs_settings_get_display_brightness(&pct, BACKLIGHT_DEFAULT_PERCENT) == ESP_OK) {
        esp_err_t err = bsp_display_set_backlight_brightness(pct);
        ESP_LOGI(TAG, "display brightness: %u%% (err=%d)", (unsigned)pct, err);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bsp_display_set_backlight_brightness(%u) failed: %d", (unsigned)pct, err);
        }
    }
    if (nvs_settings_get_keyboard_brightness(&pct, KEYBOARD_BL_DEFAULT_PERCENT) == ESP_OK) {
        esp_err_t err = bsp_input_set_backlight_brightness(pct);
        ESP_LOGI(TAG, "keyboard brightness: %u%% (err=%d)", (unsigned)pct, err);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bsp_input_set_backlight_brightness(%u) failed: %d", (unsigned)pct, err);
        }
    }
    if (nvs_settings_get_led_brightness(&pct, LED_BRIGHTNESS_DEFAULT_PCT) == ESP_OK) {
        esp_err_t err = bsp_led_set_brightness(pct);
        ESP_LOGI(TAG, "LED brightness: %u%% (err=%d)", (unsigned)pct, err);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bsp_led_set_brightness(%u) failed: %d", (unsigned)pct, err);
        }
    }

    // ---- Audio: read initial jack state, then apply ----
    bool inserted = false;
    esp_err_t err = gl_input_read_action(BSP_INPUT_ACTION_TYPE_AUDIO_JACK, &inserted);
    if (err == ESP_OK) {
        s_jack_inserted = inserted;
    } else {
        ESP_LOGW(TAG, "audio-jack initial read failed: %d (assuming no jack)", err);
        s_jack_inserted = false;
    }
    ESP_LOGI(TAG, "audio jack state at boot: %s", s_jack_inserted ? "inserted" : "not inserted");
    apply_audio_routing();

    ESP_LOGI(TAG, "se_hw_init: done (jack=%s vol=%u)",
             s_jack_inserted ? "headphones" : "speaker",
             (unsigned)read_active_volume());
    return ESP_OK;
}

void se_hw_on_jack_event(bool jack_inserted) {
    if (s_jack_inserted == jack_inserted) return;
    s_jack_inserted = jack_inserted;
    apply_audio_routing();
    ESP_LOGI(TAG, "audio jack %s -> vol=%u",
             jack_inserted ? "inserted" : "removed",
             (unsigned)read_active_volume());
}

void se_hw_set_volume(uint8_t percentage) {
    uint8_t   const newv = clamp_pct_int((int)percentage);
    esp_err_t       err  = s_jack_inserted ? nvs_settings_set_headphone_volume(newv)
                                           : nvs_settings_set_speaker_volume(newv);
    esp_err_t const apply_err = bsp_audio_set_volume((float)newv);
    ESP_LOGI(TAG, "set_volume: %s -> %u%% (nvs=%d apply=%d)",
             s_jack_inserted ? "headphone" : "speaker", (unsigned)newv, err, apply_err);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "persist volume failed: %d (continuing anyway)", err);
    }
    if (apply_err != ESP_OK) {
        ESP_LOGW(TAG, "bsp_audio_set_volume(%u) failed: %d", (unsigned)newv, apply_err);
    }
}

void se_hw_step_volume(int delta_percent) {
    // Relative nudge for the volume keys; delegates the clamp + persist +
    // apply to se_hw_set_volume so there is one code path.
    se_hw_set_volume(clamp_pct_int((int)read_active_volume() + delta_percent));
}

// ---- Settings-menu accessors ----------------------------------------

uint8_t se_hw_get_volume(void) {
    return read_active_volume();
}

uint8_t se_hw_get_display_brightness(void) {
    uint8_t v = BACKLIGHT_DEFAULT_PERCENT;
    nvs_settings_get_display_brightness(&v, BACKLIGHT_DEFAULT_PERCENT);
    return v;
}

void se_hw_set_display_brightness(uint8_t percentage) {
    uint8_t   const v   = clamp_pct_min(percentage, SE_HW_DISPLAY_BRIGHTNESS_MIN);
    esp_err_t const ne  = nvs_settings_set_display_brightness(v);
    esp_err_t const ae  = bsp_display_set_backlight_brightness(v);
    ESP_LOGI(TAG, "set display brightness: %u%% (nvs=%d apply=%d)", (unsigned)v, ne, ae);
    if (ne != ESP_OK) ESP_LOGW(TAG, "persist display brightness failed: %d", ne);
    if (ae != ESP_OK) ESP_LOGW(TAG, "bsp_display_set_backlight_brightness(%u) failed: %d", (unsigned)v, ae);
}

uint8_t se_hw_get_keyboard_brightness(void) {
    uint8_t v = KEYBOARD_BL_DEFAULT_PERCENT;
    nvs_settings_get_keyboard_brightness(&v, KEYBOARD_BL_DEFAULT_PERCENT);
    return v;
}

void se_hw_set_keyboard_brightness(uint8_t percentage) {
    uint8_t   const v  = clamp_pct_int((int)percentage);
    esp_err_t const ne = nvs_settings_set_keyboard_brightness(v);
    esp_err_t const ae = bsp_input_set_backlight_brightness(v);
    ESP_LOGI(TAG, "set keyboard brightness: %u%% (nvs=%d apply=%d)", (unsigned)v, ne, ae);
    if (ne != ESP_OK) ESP_LOGW(TAG, "persist keyboard brightness failed: %d", ne);
    if (ae != ESP_OK) ESP_LOGW(TAG, "bsp_input_set_backlight_brightness(%u) failed: %d", (unsigned)v, ae);
}

uint8_t se_hw_get_led_brightness(void) {
    uint8_t v = LED_BRIGHTNESS_DEFAULT_PCT;
    nvs_settings_get_led_brightness(&v, LED_BRIGHTNESS_DEFAULT_PCT);
    return v;
}

void se_hw_set_led_brightness(uint8_t percentage) {
    uint8_t   const v  = clamp_pct_int((int)percentage);
    esp_err_t const ne = nvs_settings_set_led_brightness(v);
    esp_err_t const ae = bsp_led_set_brightness(v);
    ESP_LOGI(TAG, "set LED brightness: %u%% (nvs=%d apply=%d)", (unsigned)v, ne, ae);
    if (ne != ESP_OK) ESP_LOGW(TAG, "persist LED brightness failed: %d", ne);
    if (ae != ESP_OK) ESP_LOGW(TAG, "bsp_led_set_brightness(%u) failed: %d", (unsigned)v, ae);
}
