/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "audio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "audio";

static audio_stt_callback_t s_stt_callback = NULL;
static audio_playback_complete_callback_t s_playback_callback = NULL;
static audio_mic_level_callback_t s_mic_level_callback = NULL;

static bool s_is_monitoring = false;
static TaskHandle_t s_monitor_task = NULL;

#define MONITOR_INTERVAL_MS 50
#define MAX_RECORD_DURATION_MS 5000
#define SAMPLE_RATE 16000
#define CHANNELS 1
#define BITS_PER_SAMPLE 16

static void monitor_task(void *pvParam)
{
    ESP_LOGI(TAG, "Mic monitor task started");
    
    while (s_is_monitoring) {
        int level = (esp_random() % 40) + (s_is_monitoring ? 20 : 0);
        
        if (s_mic_level_callback != NULL) {
            s_mic_level_callback(level);
        }
        
        vTaskDelay(pdMS_TO_TICKS(MONITOR_INTERVAL_MS));
    }
    
    ESP_LOGI(TAG, "Mic monitor task stopped");
    vTaskDelete(NULL);
}

esp_err_t audio_init(void)
{
    ESP_LOGI(TAG, "Audio initialized");
    return ESP_OK;
}

esp_err_t audio_start_stt(audio_stt_callback_t callback)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    s_stt_callback = callback;
    ESP_LOGI(TAG, "STT started");
    
    return ESP_OK;
}

esp_err_t audio_stop_stt(void)
{
    s_stt_callback = NULL;
    ESP_LOGI(TAG, "STT stopped");
    return ESP_OK;
}

esp_err_t audio_play_tts(const uint8_t *audio_data, int audio_len)
{
    if (audio_data == NULL || audio_len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Playing TTS audio, length: %d", audio_len);
    
    if (s_playback_callback != NULL) {
        s_playback_callback();
    }
    
    return ESP_OK;
}

esp_err_t audio_stop_tts(void)
{
    ESP_LOGI(TAG, "TTS stopped");
    return ESP_OK;
}

esp_err_t audio_set_volume(int volume)
{
    if (volume < 0 || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Volume set to %d", volume);
    return ESP_OK;
}

void audio_register_playback_callback(audio_playback_complete_callback_t callback)
{
    s_playback_callback = callback;
}

esp_err_t audio_debug_start_monitor(void)
{
    if (s_is_monitoring) {
        ESP_LOGW(TAG, "Monitor already running");
        return ESP_OK;
    }
    
    s_is_monitoring = true;
    xTaskCreate(monitor_task, "audio_monitor", 4096, NULL, 5, &s_monitor_task);
    
    ESP_LOGI(TAG, "Mic monitor started");
    return ESP_OK;
}

esp_err_t audio_debug_stop_monitor(void)
{
    if (!s_is_monitoring) {
        return ESP_OK;
    }
    
    s_is_monitoring = false;
    
    if (s_mic_level_callback != NULL) {
        s_mic_level_callback(0);
    }
    
    ESP_LOGI(TAG, "Mic monitor stopped");
    return ESP_OK;
}

void audio_register_mic_level_callback(audio_mic_level_callback_t callback)
{
    s_mic_level_callback = callback;
}

esp_err_t audio_debug_record_sample(uint8_t **data, int *len)
{
    if (data == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    int sample_duration_ms = 3000;
    int total_samples = (SAMPLE_RATE * sample_duration_ms) / 1000;
    *len = total_samples * (BITS_PER_SAMPLE / 8);
    
    *data = (uint8_t *)malloc(*len);
    if (*data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    int16_t *samples = (int16_t *)*data;
    for (int i = 0; i < total_samples; i++) {
        float t = (float)i / SAMPLE_RATE;
        float freq = 440.0f;
        samples[i] = (int16_t)(16000.0f * 0.5f * sinf(2.0f * 3.14159f * freq * t));
    }
    
    ESP_LOGI(TAG, "Recorded sample: %d bytes", *len);
    return ESP_OK;
}

esp_err_t audio_debug_play_test_audio(void)
{
    ESP_LOGI(TAG, "Playing test audio");
    
    int sample_duration_ms = 1000;
    int total_samples = (SAMPLE_RATE * sample_duration_ms) / 1000;
    int len = total_samples * (BITS_PER_SAMPLE / 8);
    
    uint8_t *data = (uint8_t *)malloc(len);
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    int16_t *samples = (int16_t *)data;
    for (int i = 0; i < total_samples; i++) {
        float t = (float)i / SAMPLE_RATE;
        samples[i] = (int16_t)(16000.0f * 0.3f * sinf(2.0f * 3.14159f * 440.0f * t));
    }
    
    if (s_playback_callback != NULL) {
        s_playback_callback();
    }
    
    free(data);
    return ESP_OK;
}
