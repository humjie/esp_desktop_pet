#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "bsp/esp-box-3.h"
#include "bsp_board.h"
#include "esp_codec_dev.h"
#include "mbedtls/base64.h"
#include "ui.h"

static const char *TAG = "deskpet_audio";

static esp_codec_dev_handle_t s_mic_dev = NULL;
static esp_codec_dev_handle_t s_spk_dev = NULL;
static TaskHandle_t s_mic_task_handle = NULL;
static TaskHandle_t s_spk_task_handle = NULL;
static SemaphoreHandle_t s_spk_mutex = NULL;
static RingbufHandle_t s_spk_ringbuf = NULL;

#define AUDIO_SAMPLE_RATE     (16000)
#define SAMPLES_PER_FRAME     (512)
#define STEREO_SAMPLES        (SAMPLES_PER_FRAME * 2)
#define BYTES_PER_STEREO_FRAME (STEREO_SAMPLES * sizeof(int16_t))
#define BYTES_PER_MONO_FRAME   (SAMPLES_PER_FRAME * sizeof(int16_t))
#define MAX_RECORD_FRAMES     (110)  // ~3.5 seconds max speech
#define SILENCE_FRAMES_LIMIT  (22)   // ~700ms silence to cut off

/* Statically allocated buffers to preserve task stack memory */
static int16_t s_stereo_buf[STEREO_SAMPLES];
static int16_t s_mono_buf[SAMPLES_PER_FRAME];
static char s_b64_buf[1400];

static volatile bool s_speaker_active = false;
static volatile bool s_chime_playing = false;
static volatile bool s_is_music_mode = false;
static volatile uint32_t s_spk_cur_rms = 0;
static volatile TickType_t s_last_spk_play_ticks = 0;
static int s_spk_volume = 70; // 70% volume for clean audio without mic saturation

/* Generate both bells in bounded chunks rather than allocating 11-13 KB.
 * An oscillator recurrence avoids a transcendental function for each sample. */
static void audio_play_ring(bool confirmation)
{
    if (!s_spk_dev || !s_spk_mutex) return;
    s_chime_playing = true;
    s_speaker_active = true;
    if (xSemaphoreTake(s_spk_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        s_chime_playing = false;
        s_speaker_active = false;
        return;
    }

    const int split = confirmation ? 1440 : 1280;
    const int total = confirmation ? 3200 : 2880;
    const float amplitude = confirmation ? 9500.0f : 7500.0f;
    const float taper = confirmation ? 0.20f : 0.25f;
    const float frequencies[2] = {
        confirmation ? 1046.50f : 880.00f,
        confirmation ? 1567.98f : 1318.51f,
    };
    float sin_step[2], cos_step[2];
    for (int note = 0; note < 2; note++) {
        float step = 2.0f * (float)M_PI * frequencies[note] / AUDIO_SAMPLE_RATE;
        sin_step[note] = sinf(step);
        cos_step[note] = cosf(step);
    }
    float sine = 0.0f, cosine = 1.0f;
    int16_t stereo[256 * 2];
    for (int offset = 0; offset < total; offset += 256) {
        int count = total - offset;
        if (count > 256) count = 256;
        for (int j = 0; j < count; j++) {
            int i = offset + j;
            int note = i >= split;
            if (i == split) {
                float phase = 2.0f * (float)M_PI * frequencies[1] * split / AUDIO_SAMPLE_RATE;
                sine = sinf(phase);
                cosine = cosf(phase);
            }
            float envelope = note ? 1.0f - (float)(i - split) / (total - split)
                                  : 1.0f - taper * (float)i / split;
            int16_t value = (int16_t)(amplitude * envelope * sine);
            stereo[j * 2] = stereo[j * 2 + 1] = value;
            float next_sine = sine * cos_step[note] + cosine * sin_step[note];
            cosine = cosine * cos_step[note] - sine * sin_step[note];
            sine = next_sine;
        }
        esp_codec_dev_write(s_spk_dev, stereo, count * 2 * sizeof(int16_t));
    }
    xSemaphoreGive(s_spk_mutex);
    vTaskDelay(pdMS_TO_TICKS(120));
    s_chime_playing = false;
    s_speaker_active = false;
}

/* Soft wake bell: A5 -> E6, 180 ms. */
void audio_play_soft_wake_ring(void)
{
    audio_play_ring(false);
}

/* Confirmation bell: C6 -> G6, 200 ms. */
void audio_play_confirm_ring(void)
{
    audio_play_ring(true);
}

void audio_play_chime(void)
{
    audio_play_confirm_ring();
}

void audio_set_music_mode(bool enable)
{
    s_is_music_mode = enable;
    if (!enable) {
        s_spk_cur_rms = 0;
    }
    ESP_LOGI(TAG, "Audio music mode %s", enable ? "ENABLED (echo-aware VAD active)" : "DISABLED");
}

bool audio_is_music_mode(void)
{
    return s_is_music_mode;
}

static void audio_spk_task(void *pvParameters)
{
    (void)pvParameters;
    static int16_t s_spk_stereo_chunk[512 * 2]; // 512 stereo samples (2048 bytes)

    ESP_LOGI(TAG, "Audio speaker playback task started (16kHz mono->stereo playback)");

    while (1) {
        size_t item_size = 0;
        /* Pull up to 512 mono samples (1024 bytes) from ring buffer */
        /* Incoming PCM wakes the task immediately; no polling while silent. */
        TickType_t wait = s_speaker_active ? pdMS_TO_TICKS(300) : portMAX_DELAY;
        void *data = xRingbufferReceiveUpTo(s_spk_ringbuf, &item_size, wait, 1024);
        if (data && item_size > 0) {
            s_speaker_active = true;
            s_last_spk_play_ticks = xTaskGetTickCount();

            int mono_samples = item_size / sizeof(int16_t);
            if (mono_samples > 512) mono_samples = 512;
            int16_t *mono = (int16_t *)data;

            /* Compute real-time RMS energy of output audio */
            int64_t spk_sum_sq = 0;
            for (int i = 0; i < mono_samples; i++) {
                int32_t s = mono[i];
                spk_sum_sq += s * s;
                s_spk_stereo_chunk[i * 2]     = s;
                s_spk_stereo_chunk[i * 2 + 1] = s;
            }
            s_spk_cur_rms = (uint32_t)sqrtf((float)(spk_sum_sq / mono_samples));
            vRingbufferReturnItem(s_spk_ringbuf, data);

            if (s_spk_mutex && xSemaphoreTake(s_spk_mutex, portMAX_DELAY) == pdTRUE) {
                esp_codec_dev_write(s_spk_dev, s_spk_stereo_chunk, mono_samples * 2 * sizeof(int16_t));
                xSemaphoreGive(s_spk_mutex);
            }
        } else {
            /* If ring buffer is empty and 300ms has elapsed since last frame, speaker is idle */
            if (s_speaker_active && (xTaskGetTickCount() - s_last_spk_play_ticks > pdMS_TO_TICKS(300))) {
                s_speaker_active = false;
                s_spk_cur_rms = 0;
            }
        }
    }
}

void audio_feed_spk_base64(const char *b64_str)
{
    if (!s_spk_ringbuf || !b64_str) return;

    size_t b64_len = strlen(b64_str);
    if (b64_len == 0) return;

    static uint8_t s_pcm_decoded[1024];
    size_t out_len = 0;
    int ret = mbedtls_base64_decode(s_pcm_decoded, sizeof(s_pcm_decoded), &out_len,
                                    (const unsigned char *)b64_str, b64_len);
    if (ret == 0 && out_len > 0) {
        s_speaker_active = true;
        xRingbufferSend(s_spk_ringbuf, s_pcm_decoded, out_len, pdMS_TO_TICKS(100));
    }
}

void audio_spk_stop(void)
{
    if (!s_spk_ringbuf) return;
    size_t item_size = 0;
    while (1) {
        void *data = xRingbufferReceiveUpTo(s_spk_ringbuf, &item_size, 0, 4096);
        if (!data || item_size == 0) break;
        vRingbufferReturnItem(s_spk_ringbuf, data);
    }
    s_speaker_active = false;
    s_spk_cur_rms = 0;
    s_is_music_mode = false;
}

void audio_set_spk_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_spk_volume = volume;
    if (s_spk_dev) {
        esp_codec_dev_set_out_vol(s_spk_dev, s_spk_volume);
    }
    ESP_LOGI(TAG, "Speaker volume set to %d%%", s_spk_volume);
}

int audio_get_spk_volume(void)
{
    return s_spk_volume;
}

bool audio_is_speaker_active(void)
{
    return s_speaker_active;
}

static void audio_mic_task(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "Microphone capture & VAD task started (16kHz stereo capture, mono extraction)");

    uint32_t noise_floor = 300;
    int calib_frames = 0;
    uint32_t calib_sum = 0;
    bool is_recording = false;
    int silence_count = 0;
    int total_frames = 0;

    /* Pre-roll ring buffer (2 frames = ~64ms) to prevent clipping first syllable */
    static int16_t s_preroll_buf[2][SAMPLES_PER_FRAME];
    int preroll_idx = 0;

    while (1) {
        /* Only pause microphone during short 180ms chime tone playback */
        if (s_chime_playing) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* Also pause during pet's own short TTS spoken answers (1-2s) */
        if (!s_is_music_mode && s_speaker_active && (xTaskGetTickCount() - s_last_spk_play_ticks < pdMS_TO_TICKS(150))) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        int ret = esp_codec_dev_read(s_mic_dev, s_stereo_buf, BYTES_PER_STEREO_FRAME);
        if (ret != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (s_chime_playing) {
            continue;
        }

        /* 1. Extract primary microphone channel, apply +3.0 dB digital boost, and compute DC offset */
        int32_t sum = 0;
        for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
            /* 1.414x multiplier (+3.0 dB digital gain over +37.5 dB analog = 40.5 dB effective listening gain) */
            int32_t boosted = (int32_t)s_stereo_buf[i * 2] * 1414 / 1000;
            if (boosted > 32767) boosted = 32767;
            if (boosted < -32768) boosted = -32768;
            s_mono_buf[i] = (int16_t)boosted;
            sum += s_mono_buf[i];
        }
        int16_t dc_offset = (int16_t)(sum / SAMPLES_PER_FRAME);

        /* 2. Subtract DC bias and compute AC RMS energy */
        int64_t sum_sq = 0;
        for (int i = 0; i < SAMPLES_PER_FRAME; i++) {
            int32_t ac = (int32_t)s_mono_buf[i] - dc_offset;
            if (ac > 32767) ac = 32767;
            if (ac < -32768) ac = -32768;
            s_mono_buf[i] = (int16_t)ac;
            sum_sq += ac * ac;
        }
        uint32_t rms = (uint32_t)sqrtf((float)(sum_sq / SAMPLES_PER_FRAME));

        /* 3. Startup calibration (first 30 frames ~1s) */
        if (calib_frames < 30) {
            calib_sum += rms;
            calib_frames++;
            if (calib_frames == 30) {
                noise_floor = calib_sum / 30;
                if (noise_floor < 150) noise_floor = 150;
                ESP_LOGI(TAG, "Audio VAD calibrated: baseline noise floor = %lu RMS", (unsigned long)noise_floor);
            }
            continue;
        }

        /* 4. Dynamic Echo-Aware Background Level:
         * Calculates expected acoustic bleed from the onboard speaker to dual microphones.
         * Measured transfer ratio on BOX-3 chassis is ~0.40x speaker RMS. */
        uint32_t spk_bleed = 0;
        if (s_is_music_mode && (xTaskGetTickCount() - s_last_spk_play_ticks < pdMS_TO_TICKS(200))) {
            spk_bleed = (s_spk_cur_rms * 40) / 100;
        }
        uint32_t effective_bg = noise_floor + spk_bleed;

        if (!is_recording) {
            /* Adapt ambient room noise floor only when music is not active */
            if (spk_bleed == 0) {
                if (rms <= noise_floor) {
                    noise_floor = (noise_floor * 15 + rms) / 16;
                } else if (rms < (noise_floor * 14) / 10) {
                    noise_floor = (noise_floor * 63 + rms) / 64;
                }
                if (noise_floor < 100) noise_floor = 100;
                if (noise_floor > 5000) noise_floor = 5000;
            }

            /* Store current frame into pre-roll buffer */
            memcpy(s_preroll_buf[preroll_idx], s_mono_buf, BYTES_PER_MONO_FRAME);
            preroll_idx = (preroll_idx + 1) % 2;

            /* Trigger threshold:
             * In music mode: requires voice peak to exceed (effective_bg + 650).
             * In quiet mode: ~2.3x ambient noise floor or +450 RMS. */
            uint32_t trigger_thresh;
            if (s_is_music_mode && spk_bleed > 0) {
                trigger_thresh = effective_bg + 650;
                if (trigger_thresh < 1200) trigger_thresh = 1200;
            } else {
                trigger_thresh = (noise_floor * 23) / 10;
                if (trigger_thresh < noise_floor + 450) trigger_thresh = noise_floor + 450;
                if (trigger_thresh < 800) trigger_thresh = 800;
            }

            if (rms > trigger_thresh) {
                is_recording = true;
                silence_count = 0;
                total_frames = 0;

                printf("AUD,START,%d,16,1\n", AUDIO_SAMPLE_RATE);
                fflush(stdout);

                /* Send older pre-roll frame first to preserve initial consonant */
                int older_idx = preroll_idx; // points to the oldest frame in circular buffer
                size_t olen = 0;
                mbedtls_base64_encode((unsigned char *)s_b64_buf, sizeof(s_b64_buf), &olen,
                                      (const unsigned char *)s_preroll_buf[older_idx], BYTES_PER_MONO_FRAME);
                s_b64_buf[olen] = '\0';
                printf("AUD,D,%s\n", s_b64_buf);
                fflush(stdout);

                /* Send current speech chunk */
                olen = 0;
                mbedtls_base64_encode((unsigned char *)s_b64_buf, sizeof(s_b64_buf), &olen,
                                      (const unsigned char *)s_mono_buf, BYTES_PER_MONO_FRAME);
                s_b64_buf[olen] = '\0';
                printf("AUD,D,%s\n", s_b64_buf);
                fflush(stdout);
            }
        } else {
            /* Currently streaming speech */
            total_frames++;

            size_t olen = 0;
            mbedtls_base64_encode((unsigned char *)s_b64_buf, sizeof(s_b64_buf), &olen,
                                  (const unsigned char *)s_mono_buf, BYTES_PER_MONO_FRAME);
            s_b64_buf[olen] = '\0';
            printf("AUD,D,%s\n", s_b64_buf);
            fflush(stdout);

            uint32_t cutoff_thresh;
            if (s_is_music_mode && spk_bleed > 0) {
                cutoff_thresh = effective_bg + 250;
            } else {
                cutoff_thresh = (noise_floor * 14) / 10;
                if (cutoff_thresh < noise_floor + 180) cutoff_thresh = noise_floor + 180;
                if (cutoff_thresh < 550) cutoff_thresh = 550;
            }

            if (rms < cutoff_thresh) {
                silence_count++;
            } else {
                silence_count = 0;
            }

            /* End recording when user stops speaking or maximum duration reached */
            if (silence_count >= SILENCE_FRAMES_LIMIT || total_frames >= MAX_RECORD_FRAMES) {
                printf("AUD,END\n");
                fflush(stdout);

                is_recording = false;
                silence_count = 0;
                total_frames = 0;

                /* Cooldown to avoid immediately re-triggering on room reverberation */
                vTaskDelay(pdMS_TO_TICKS(250));
            }
        }
    }
}

esp_err_t audio_init(void)
{
    ESP_LOGI(TAG, "Initializing Audio codecs (ES8311 speaker + ES7210 dual mic)...");

    s_spk_mutex = xSemaphoreCreateMutex();
    if (!s_spk_mutex) {
        ESP_LOGE(TAG, "Failed to create speaker mutex");
        return ESP_FAIL;
    }

    /* 32KB byte buffer holds ~1 second of 16kHz 16-bit mono audio */
    s_spk_ringbuf = xRingbufferCreate(32768, RINGBUF_TYPE_BYTEBUF);
    if (!s_spk_ringbuf) {
        ESP_LOGE(TAG, "Failed to create speaker ringbuffer");
        return ESP_FAIL;
    }

    /* Initialize ES8311 speaker DAC */
    s_spk_dev = bsp_audio_codec_speaker_init();
    if (!s_spk_dev) {
        ESP_LOGE(TAG, "Speaker codec init failed");
        return ESP_FAIL;
    }

    /* Initialize ES7210 dual microphone ADC (reuses shared I2S data interface) */
    s_mic_dev = bsp_audio_codec_microphone_init();
    if (!s_mic_dev) {
        ESP_LOGE(TAG, "Microphone codec init failed");
        return ESP_FAIL;
    }

    /* Configure duplex 16kHz, 16-bit, 2-channel format */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .channel = 2,
        .bits_per_sample = 16,
    };

    esp_codec_dev_close(s_spk_dev);
    esp_codec_dev_close(s_mic_dev);

    esp_codec_dev_set_in_gain(s_mic_dev, 37.5f); // Maximum sensitivity hardware analog gain (+37.5 dB)
    esp_codec_dev_set_out_vol(s_spk_dev, s_spk_volume);    // Default 70% volume

    esp_err_t err = esp_codec_dev_open(s_spk_dev, &fs);
    if (err != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open speaker dev: %d", err);
        return ESP_FAIL;
    }

    err = esp_codec_dev_open(s_mic_dev, &fs);
    if (err != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open microphone dev: %d", err);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Audio subsystem ready (16kHz stereo duplex)");

    /* Start background mic capture and VAD task on Core 0 */
    xTaskCreatePinnedToCore(audio_mic_task, "audio_mic", 8192, NULL, 5, &s_mic_task_handle, 0);

    /* Start background speaker audio playback task on Core 0 */
    xTaskCreatePinnedToCore(audio_spk_task, "audio_spk", 4096, NULL, 6, &s_spk_task_handle, 0);

    return ESP_OK;
}
