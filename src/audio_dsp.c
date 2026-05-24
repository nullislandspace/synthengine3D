#include "se_audio_dsp.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static char const TAG[] = "audio_dsp";

// 1024-entry sine table, int16. Top 10 bits of `phase` index it.
// Stored in PSRAM since we have plenty and only read it; the
// table is 2 KiB and fits in cache lines comfortably on the P4.
#define SINE_TABLE_SIZE 1024
static int16_t* s_sine_table = NULL;

static SemaphoreHandle_t s_init_mutex = NULL;

static void ensure_mutex(void) {
    // FreeRTOS mutexes can't be statically initialised, so guard
    // with a once-style check; safe under our boot ordering where
    // audio_dsp_init() is called from app_main before any task
    // touches the DSP.
    if (s_init_mutex == NULL) {
        s_init_mutex = xSemaphoreCreateMutex();
    }
}

esp_err_t audio_dsp_init(void) {
    ensure_mutex();
    if (s_init_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to allocate init mutex");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_init_mutex, portMAX_DELAY);
    if (s_sine_table != NULL) {
        xSemaphoreGive(s_init_mutex);
        return ESP_OK;  // already initialised
    }

    s_sine_table = (int16_t*)heap_caps_malloc(SINE_TABLE_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s_sine_table == NULL) {
        ESP_LOGE(TAG, "Failed to allocate sine table");
        xSemaphoreGive(s_init_mutex);
        return ESP_ERR_NO_MEM;
    }

    float const k = 2.0f * (float)M_PI / (float)SINE_TABLE_SIZE;
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        s_sine_table[i] = (int16_t)(sinf((float)i * k) * 32767.0f);
    }

    ESP_LOGI(TAG, "DSP initialised (sine table %d entries)", SINE_TABLE_SIZE);
    xSemaphoreGive(s_init_mutex);
    return ESP_OK;
}

float audio_dsp_sin(uint32_t phase) {
    if (s_sine_table == NULL) return 0.0f;
    uint32_t const idx = phase >> 22;  // top 10 bits → 0..1023
    return (float)s_sine_table[idx] * (1.0f / 32767.0f);
}

// ---------------------------------------------------------------
// ADSR
// ---------------------------------------------------------------

void audio_env_configure(audio_env_t* e, float attack_s, float decay_s,
                         float sustain_level, float release_s) {
    // Avoid divide-by-zero on a "zero-length" stage by clamping to
    // one sample minimum — caller can just set the level directly
    // if they really want an instant snap.
    float const sr = (float)AUDIO_SAMPLE_RATE_HZ;
    if (attack_s  < 1.0f / sr) attack_s  = 1.0f / sr;
    if (decay_s   < 1.0f / sr) decay_s   = 1.0f / sr;
    if (release_s < 1.0f / sr) release_s = 1.0f / sr;
    if (sustain_level < 0.0f) sustain_level = 0.0f;
    if (sustain_level > 1.0f) sustain_level = 1.0f;

    e->stage              = AUDIO_ENV_IDLE;
    e->level              = 0.0f;
    e->sustain            = sustain_level;
    e->attack_per_sample  = 1.0f / (attack_s  * sr);
    e->decay_per_sample   = (1.0f - sustain_level) / (decay_s * sr);
    e->release_per_sample = 1.0f / (release_s * sr);
}

void audio_env_trigger(audio_env_t* e) {
    e->stage = AUDIO_ENV_ATTACK;
    e->level = 0.0f;
}

void audio_env_release(audio_env_t* e) {
    if (e->stage == AUDIO_ENV_IDLE) return;
    e->stage = AUDIO_ENV_RELEASE;
}

float audio_env_tick(audio_env_t* e) {
    switch (e->stage) {
        case AUDIO_ENV_IDLE:
            return 0.0f;
        case AUDIO_ENV_ATTACK:
            e->level += e->attack_per_sample;
            if (e->level >= 1.0f) {
                e->level = 1.0f;
                e->stage = AUDIO_ENV_DECAY;
            }
            return e->level;
        case AUDIO_ENV_DECAY:
            e->level -= e->decay_per_sample;
            if (e->level <= e->sustain) {
                e->level = e->sustain;
                // Sustain=0 turns the envelope into a one-shot:
                // when DECAY meets the floor we drop straight to
                // IDLE so the owning voice can report finished.
                // Sustained envelopes (sustain>0) continue to
                // hold until audio_env_release() is called.
                e->stage = (e->sustain <= 0.0f) ? AUDIO_ENV_IDLE : AUDIO_ENV_SUSTAIN;
            }
            return e->level;
        case AUDIO_ENV_SUSTAIN:
            return e->level;
        case AUDIO_ENV_RELEASE:
            e->level -= e->release_per_sample;
            if (e->level <= 0.0f) {
                e->level = 0.0f;
                e->stage = AUDIO_ENV_IDLE;
            }
            return e->level;
    }
    return 0.0f;
}

// ---------------------------------------------------------------
// Biquad (RBJ cookbook)
// ---------------------------------------------------------------

// Coefficient update only — preserves the filter's x/y history so a
// per-chunk retune (e.g. engine-hum LPF cutoff tracking ship speed)
// stays continuous across chunk boundaries. Resetting the history
// here causes audible clicks at the rate filters are retuned (every
// ~11.6 ms at our chunk size) because the filter's output suddenly
// goes from "filtered sample N" to "filtered sample N with zero past
// state". Callers that genuinely need a fresh start (initial voice
// setup of a voice whose backing memory isn't already zeroed) should
// call `audio_biquad_reset` explicitly after the configure call.
static void biquad_set(audio_biquad_t* f, float b0, float b1, float b2, float a0, float a1, float a2) {
    float const inv = 1.0f / a0;
    f->b0 = b0 * inv;
    f->b1 = b1 * inv;
    f->b2 = b2 * inv;
    f->a1 = a1 * inv;
    f->a2 = a2 * inv;
}

void audio_biquad_lpf(audio_biquad_t* f, float fc, float q) {
    if (q < 0.1f) q = 0.1f;
    float const sr    = (float)AUDIO_SAMPLE_RATE_HZ;
    float const w0    = 2.0f * (float)M_PI * fc / sr;
    float const cosw0 = cosf(w0);
    float const sinw0 = sinf(w0);
    float const alpha = sinw0 / (2.0f * q);
    float const b0 = (1.0f - cosw0) * 0.5f;
    float const b1 = 1.0f - cosw0;
    float const b2 = (1.0f - cosw0) * 0.5f;
    float const a0 = 1.0f + alpha;
    float const a1 = -2.0f * cosw0;
    float const a2 = 1.0f - alpha;
    biquad_set(f, b0, b1, b2, a0, a1, a2);
}

void audio_biquad_hpf(audio_biquad_t* f, float fc, float q) {
    if (q < 0.1f) q = 0.1f;
    float const sr    = (float)AUDIO_SAMPLE_RATE_HZ;
    float const w0    = 2.0f * (float)M_PI * fc / sr;
    float const cosw0 = cosf(w0);
    float const sinw0 = sinf(w0);
    float const alpha = sinw0 / (2.0f * q);
    float const b0 =  (1.0f + cosw0) * 0.5f;
    float const b1 = -(1.0f + cosw0);
    float const b2 =  (1.0f + cosw0) * 0.5f;
    float const a0 = 1.0f + alpha;
    float const a1 = -2.0f * cosw0;
    float const a2 = 1.0f - alpha;
    biquad_set(f, b0, b1, b2, a0, a1, a2);
}

void audio_biquad_bpf(audio_biquad_t* f, float fc, float q) {
    if (q < 0.1f) q = 0.1f;
    float const sr    = (float)AUDIO_SAMPLE_RATE_HZ;
    float const w0    = 2.0f * (float)M_PI * fc / sr;
    float const cosw0 = cosf(w0);
    float const sinw0 = sinf(w0);
    float const alpha = sinw0 / (2.0f * q);
    // Constant 0-dB peak gain bandpass form.
    float const b0 =  alpha;
    float const b1 =  0.0f;
    float const b2 = -alpha;
    float const a0 = 1.0f + alpha;
    float const a1 = -2.0f * cosw0;
    float const a2 = 1.0f - alpha;
    biquad_set(f, b0, b1, b2, a0, a1, a2);
}
