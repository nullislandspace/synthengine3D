#include "se_audio.h"

#include "se_audio_dsp.h"
#include "se_config.h"

#include "bsp/audio.h"
#include "bsp/input.h"
#include "driver/i2s_common.h"
#include "driver/i2s_types.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdint.h>
#include <string.h>

static char const TAG[] = "audio_mixer";

// Pipeline format.
#define MIXER_FRAME_BYTES   4                              // s16 stereo
#define MIXER_CHUNK_FRAMES  256                            // ~11.6 ms @ 22050
#define MIXER_CHUNK_SAMPLES (MIXER_CHUNK_FRAMES * 2)       // L+R
#define MIXER_CHUNK_BYTES   (MIXER_CHUNK_FRAMES * MIXER_FRAME_BYTES)

// Master gain factors (overridable engine defaults in se_config.h).
// Both are applied in Q15 fixed-point at mix-down time, so the
// per-voice `*_AMP` constants in each `sfx_*.c` stay tuned to relative
// loudness between effects and these set the overall balance.
//
// See the "Audio gain staging" block in se_config.h for the headroom
// math (target: 5 concurrent SFX + music + hum without hard-clipping
// the int16 mixer accumulator).
#define MIXER_MUSIC_GAIN_Q15 ((int32_t)(AUDIO_MUSIC_GAIN * 32768.0f))
#define MIXER_SFX_GAIN_Q15   ((int32_t)(AUDIO_SFX_GAIN   * 32768.0f))

// Output gates pushed by the host app (at startup and on toggle) via
// audio_mixer_set_music_enabled() / audio_mixer_set_group_enabled(); the
// mixer task reads these instead of reaching into app/NVS settings, so
// the engine carries no app-config dependency. The music slot has its
// own gate; SFX voices are gated by their app-defined `group` index, so
// the engine knows nothing about what any group *means*. Plain bools
// written from the app thread and read on the audio task — a torn read
// just means one chunk uses the stale value, which is harmless.
static volatile bool g_music_gate = true;
static volatile bool g_group_gate[SE_AUDIO_SFX_GROUP_COUNT] =
    { [0 ... SE_AUDIO_SFX_GROUP_COUNT - 1] = true };   // groups default enabled

// Number of consecutive silent chunks the mixer pushes through the
// I2S DMA queue before powering down. ~46 ms covers the worst case
// of DMA queue + codec latency without chopping the tail off a
// just-finished SFX.
#define MIXER_DRAIN_CHUNKS 4   // 4 * 256 frames @ 22050 ≈ 46 ms

// Voice slots — N concurrent SFX. Engine hum + scrape can be
// persistent; one-shots flow through whatever's free. 8 is
// generous: typical worst case is hum + scrape + one or two
// one-shots overlapping.
#define MIXER_VOICE_SLOTS 8

#define MIXER_TASK_STACK_WORDS 6144
#define MIXER_TASK_PRIORITY    (configMAX_PRIORITIES - 2)  // just under timer/IDLE-pinning priorities

typedef struct {
    sfx_voice_t* voice;
} voice_slot_t;

static music_source_t* g_music = NULL;
static voice_slot_t    g_voices[MIXER_VOICE_SLOTS];

static SemaphoreHandle_t g_slots_mutex = NULL;
static i2s_chan_handle_t g_i2s         = NULL;
static TaskHandle_t      g_mixer_task  = NULL;
static bool              g_initialised = false;
static volatile bool     g_shutdown    = false;
static bool              g_powered_on  = true;

// Scratch buffers — static so they don't burn task-stack space.
// `g_mix_*` is the per-source render target; `g_accum` is the
// signed accumulator before saturation; `g_out` is the int16
// chunk we hand to I2S.
static int16_t g_mix_music[MIXER_CHUNK_SAMPLES];
static int16_t g_mix_voice[MIXER_CHUNK_SAMPLES];
static int32_t g_accum[MIXER_CHUNK_SAMPLES];
static int16_t g_out[MIXER_CHUNK_SAMPLES];

// ---------------------------------------------------------------
// Power management — only called from the mixer task.
// ---------------------------------------------------------------

static void power_up(void) {
    if (g_powered_on) return;

    esp_err_t err = i2s_channel_enable(g_i2s);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "power_up: i2s_channel_enable failed: %s", esp_err_to_name(err));
    }

    bool jack_inserted = false;
    if (bsp_input_read_action(BSP_INPUT_ACTION_TYPE_AUDIO_JACK, &jack_inserted) != ESP_OK) {
        jack_inserted = false;
    }
    bsp_audio_set_amplifier(!jack_inserted);

    g_powered_on = true;
    ESP_LOGI(TAG, "power_up: I2S enabled, amp=%s (jack=%s)",
             jack_inserted ? "off" : "on", jack_inserted ? "in" : "out");
}

static void power_down(void) {
    if (!g_powered_on) return;

    bsp_audio_set_amplifier(false);

    esp_err_t err = i2s_channel_disable(g_i2s);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power_down: i2s_channel_disable failed: %s", esp_err_to_name(err));
    }

    g_powered_on = false;
    ESP_LOGI(TAG, "power_down: amp+I2S off (idle drain complete)");
}

// ---------------------------------------------------------------
// Voice slot helpers — caller holds g_slots_mutex.
// ---------------------------------------------------------------

static int find_free_slot_locked(void) {
    for (int i = 0; i < MIXER_VOICE_SLOTS; i++) {
        if (g_voices[i].voice == NULL) return i;
    }
    return -1;
}

static void reap_finished_voices_locked(void) {
    for (int i = 0; i < MIXER_VOICE_SLOTS; i++) {
        sfx_voice_t* v = g_voices[i].voice;
        if (v != NULL && v->finished) {
            if (v->shutdown) v->shutdown(v);
            g_voices[i].voice = NULL;
        }
    }
}

// ---------------------------------------------------------------
// Mixer task
// ---------------------------------------------------------------

static void mixer_task_fn(void* arg) {
    (void)arg;
    int  silence_chunks         = 0;
    bool logged_first_audio_out = false;

    ESP_LOGI(TAG, "mixer task started");

    while (1) {
        if (g_shutdown) {
            // Final shutdown path: drop into a permanent block.
            // amplifier + I2S are already muted/disabled in
            // audio_mixer_shutdown().
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        bool const music_gate = g_music_gate;
        int active_sources = 0;

        memset(g_accum, 0, sizeof(g_accum));

        // ---- Music render + scale into accum at 30% ----
        xSemaphoreTake(g_slots_mutex, portMAX_DELAY);
        music_source_t* music = g_music;
        if (music_gate && music != NULL && music->render != NULL) {
            memset(g_mix_music, 0, sizeof(g_mix_music));
            music->render(music, g_mix_music, MIXER_CHUNK_FRAMES);
            for (size_t j = 0; j < MIXER_CHUNK_SAMPLES; j++) {
                g_accum[j] += ((int32_t)g_mix_music[j] * MIXER_MUSIC_GAIN_Q15) >> 15;
            }
            active_sources++;
        }

        // ---- SFX voices ----
        // Gating is *per voice* by its app-defined `group` index — the
        // engine doesn't know what any group means; the host enables /
        // disables groups via audio_mixer_set_group_enabled(). An
        // out-of-range group is treated as always-on.
        //
        // SFX master gain is applied at sum-in time (Q15 fixed
        // point). Per-voice `*_AMP` constants in each sfx_*.c set
        // relative loudness; this knob sets the overall SFX bus
        // level against the music + clip ceiling. See se_config.h
        // for the 5-concurrent-voice headroom budget.
        for (int i = 0; i < MIXER_VOICE_SLOTS; i++) {
            sfx_voice_t* v = g_voices[i].voice;
            if (v == NULL || v->finished || v->render == NULL) continue;

            bool const allowed = (v->group < SE_AUDIO_SFX_GROUP_COUNT)
                                     ? g_group_gate[v->group] : true;
            if (!allowed) continue;

            memset(g_mix_voice, 0, sizeof(g_mix_voice));
            v->render(v, g_mix_voice, MIXER_CHUNK_FRAMES);
            for (size_t j = 0; j < MIXER_CHUNK_SAMPLES; j++) {
                g_accum[j] += ((int32_t)g_mix_voice[j] * MIXER_SFX_GAIN_Q15) >> 15;
            }
            active_sources++;
        }

        // ---- Reap finished SFX voices ----
        reap_finished_voices_locked();
        xSemaphoreGive(g_slots_mutex);

        // ---- Idle handling ----
        if (active_sources == 0) {
            if (!g_powered_on) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                continue;
            }
            if (silence_chunks < MIXER_DRAIN_CHUNKS) {
                memset(g_out, 0, sizeof(g_out));
                size_t written = 0;
                i2s_channel_write(g_i2s, g_out, sizeof(g_out), &written, portMAX_DELAY);
                silence_chunks++;
                continue;
            }
            power_down();
            silence_chunks = 0;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        // Power back up before the first non-silent chunk goes out.
        if (!g_powered_on) power_up();
        silence_chunks = 0;

        // ---- Saturate accum → out + peak sampling ----
        int32_t peak = 0;
        for (size_t j = 0; j < MIXER_CHUNK_SAMPLES; j++) {
            int32_t s = g_accum[j];
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            g_out[j] = (int16_t)s;
            int32_t a = s < 0 ? -s : s;
            if (a > peak) peak = a;
        }

        size_t    written = 0;
        esp_err_t werr    = i2s_channel_write(g_i2s, g_out, sizeof(g_out), &written, portMAX_DELAY);
        if (!logged_first_audio_out) {
            ESP_LOGI(TAG, "first audible chunk: write_err=%s bytes=%u peak=%ld sources=%d",
                     esp_err_to_name(werr), (unsigned)written, (long)peak, active_sources);
            logged_first_audio_out = true;
        } else if (werr != ESP_OK) {
            // Don't spam every chunk; only log unexpected errors.
            ESP_LOGW(TAG, "i2s_channel_write: %s (bytes=%u)", esp_err_to_name(werr), (unsigned)written);
        }
    }
}

// ---------------------------------------------------------------
// Public API
// ---------------------------------------------------------------

esp_err_t audio_mixer_init(void) {
    if (g_initialised) return ESP_OK;

    ESP_LOGI(TAG, "audio_mixer_init: begin");

    esp_err_t err = audio_dsp_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_dsp_init failed: %d", err);
        return err;
    }

    // The BSP enables the I2S channel inside `bsp_device_initialize()`
    // at whatever sample rate the boot config picked (typically
    // 16 kHz). The ESP-IDF i2s_std driver rejects clock reconfigure
    // on an enabled channel with ESP_ERR_INVALID_STATE (259), so we
    // have to disable → set rate → re-enable. Grab the handle first
    // so we can disable it ourselves.
    err = bsp_audio_get_i2s_handle(&g_i2s);
    if (err != ESP_OK || g_i2s == NULL) {
        ESP_LOGE(TAG, "bsp_audio_get_i2s_handle failed: %d (handle=%p)", err, g_i2s);
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
    ESP_LOGI(TAG, "bsp_audio_get_i2s_handle ok: handle=%p", g_i2s);

    err = i2s_channel_disable(g_i2s);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "i2s_channel_disable before set_rate failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "i2s_channel_disable ok (pre-rate-change)");
    }

    err = bsp_audio_set_rate(AUDIO_SAMPLE_RATE_HZ);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_set_rate(%u) failed: %d", AUDIO_SAMPLE_RATE_HZ, err);
        // Try to leave the I2S channel enabled so any subsequent
        // BSP audio path still works at the old rate.
        i2s_channel_enable(g_i2s);
        return err;
    }
    ESP_LOGI(TAG, "bsp_audio_set_rate(%u) ok", AUDIO_SAMPLE_RATE_HZ);

    err = i2s_channel_enable(g_i2s);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "i2s_channel_enable ok");
    } else if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "i2s_channel_enable: already enabled");
    } else {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
    }

    // Match the launcher's amp policy: speaker on unless headphones
    // are inserted. hw_settings_init() will overwrite the volume
    // value with the launcher-persisted one immediately after.
    bool jack = false;
    if (bsp_input_read_action(BSP_INPUT_ACTION_TYPE_AUDIO_JACK, &jack) != ESP_OK) jack = false;
    err = bsp_audio_set_amplifier(!jack);
    ESP_LOGI(TAG, "initial amp policy: jack=%s amp=%s err=%d",
             jack ? "in" : "out", jack ? "off" : "on", err);

    g_slots_mutex = xSemaphoreCreateMutex();
    if (g_slots_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mixer mutex");
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < MIXER_VOICE_SLOTS; i++) g_voices[i].voice = NULL;
    g_music       = NULL;
    g_powered_on  = true;
    g_shutdown    = false;

    BaseType_t ok = xTaskCreatePinnedToCore(
        mixer_task_fn, "audio_mixer",
        MIXER_TASK_STACK_WORDS, NULL,
        MIXER_TASK_PRIORITY, &g_mixer_task,
        1  // core 1 — leaves core 0 for the render loop
    );
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mixer task");
        vSemaphoreDelete(g_slots_mutex);
        g_slots_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    g_initialised = true;
    ESP_LOGI(TAG, "audio_mixer_init: done (%d Hz, %d frames/chunk, %d voices, task=%p)",
             AUDIO_SAMPLE_RATE_HZ, MIXER_CHUNK_FRAMES, MIXER_VOICE_SLOTS, g_mixer_task);
    return ESP_OK;
}

void audio_mixer_shutdown(void) {
    if (!g_initialised) return;

    // Tell the mixer task to stop touching the hardware *before* we
    // disable I2S, so it doesn't immediately try to power it back
    // up. Clear the music slot and all voices so render passes
    // produce no work.
    g_shutdown = true;

    xSemaphoreTake(g_slots_mutex, portMAX_DELAY);
    if (g_music != NULL) {
        if (g_music->shutdown) g_music->shutdown(g_music);
        g_music = NULL;
    }
    for (int i = 0; i < MIXER_VOICE_SLOTS; i++) {
        sfx_voice_t* v = g_voices[i].voice;
        if (v != NULL) {
            if (v->shutdown) v->shutdown(v);
            g_voices[i].voice = NULL;
        }
    }
    xSemaphoreGive(g_slots_mutex);

    // Wake the mixer task so it sees the shutdown flag and parks.
    if (g_mixer_task != NULL) xTaskNotifyGive(g_mixer_task);

    // Synchronously silence the hardware right now. Subsequent
    // calls into the mixer are no-ops by design.
    bsp_audio_set_amplifier(false);
    if (g_i2s != NULL) {
        esp_err_t err = i2s_channel_disable(g_i2s);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_disable in shutdown failed: %s", esp_err_to_name(err));
        }
    }
    g_powered_on = false;
    ESP_LOGI(TAG, "Audio shutdown — amp + I2S off");
}

void audio_mixer_set_music(music_source_t* src) {
    if (!g_initialised || g_shutdown) {
        ESP_LOGW(TAG, "set_music skipped: init=%d shutdown=%d", g_initialised, g_shutdown);
        return;
    }

    music_source_t* prev = NULL;
    xSemaphoreTake(g_slots_mutex, portMAX_DELAY);
    prev    = g_music;
    g_music = src;
    xSemaphoreGive(g_slots_mutex);

    if (prev != NULL && prev->shutdown != NULL) {
        prev->shutdown(prev);
    }

    if (src != NULL && g_mixer_task != NULL) {
        xTaskNotifyGive(g_mixer_task);
    }
    ESP_LOGI(TAG, "set_music: %s (prev=%p new=%p)",
             src ? "installed" : "cleared", prev, src);
}

bool audio_mixer_register_voice(sfx_voice_t* v) {
    if (!g_initialised || g_shutdown || v == NULL) {
        ESP_LOGW(TAG, "register_voice skipped: init=%d shutdown=%d v=%p",
                 g_initialised, g_shutdown, v);
        return false;
    }

    bool placed = false;
    int  idx    = -1;
    xSemaphoreTake(g_slots_mutex, portMAX_DELAY);
    idx = find_free_slot_locked();
    if (idx >= 0) {
        v->finished        = false;
        g_voices[idx].voice = v;
        placed             = true;
    }
    xSemaphoreGive(g_slots_mutex);

    if (placed && g_mixer_task != NULL) {
        xTaskNotifyGive(g_mixer_task);
        ESP_LOGI(TAG, "register_voice: slot=%d voice=%p", idx, v);
    } else if (!placed) {
        ESP_LOGW(TAG, "register_voice: all SFX voice slots full — dropping voice");
    }
    return placed;
}

void audio_mixer_stop_voice(sfx_voice_t* v) {
    if (!g_initialised || v == NULL) return;
    // We don't unhook the slot here — the mixer task does that on
    // its next pass via reap_finished_voices_locked(). Marking the
    // flag is just a hint that lives next to the voice struct.
    v->finished = true;
}

void audio_mixer_stop_all_voices(void) {
    if (!g_initialised) return;
    xSemaphoreTake(g_slots_mutex, portMAX_DELAY);
    for (int i = 0; i < MIXER_VOICE_SLOTS; i++) {
        if (g_voices[i].voice != NULL) {
            g_voices[i].voice->finished = true;
        }
    }
    xSemaphoreGive(g_slots_mutex);
}

// Output gates pushed by the host app (see the g_*_gate declarations).
// Safe to call before audio_mixer_init() — they only set a flag the
// mixer task reads once running.
void audio_mixer_set_music_enabled(bool on) { g_music_gate = on; }

void audio_mixer_set_group_enabled(uint8_t group, bool on) {
    if (group < SE_AUDIO_SFX_GROUP_COUNT) g_group_gate[group] = on;
}
