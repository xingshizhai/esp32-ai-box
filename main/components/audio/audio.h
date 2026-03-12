/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*audio_stt_callback_t)(const char *text);
typedef void (*audio_playback_complete_callback_t)(void);
typedef void (*audio_mic_level_callback_t)(int level);

esp_err_t audio_init(void);
esp_err_t audio_start_stt(audio_stt_callback_t callback);
esp_err_t audio_stop_stt(void);
esp_err_t audio_play_tts(const uint8_t *audio_data, int audio_len);
esp_err_t audio_stop_tts(void);
esp_err_t audio_set_volume(int volume);
void audio_register_playback_callback(audio_playback_complete_callback_t callback);

esp_err_t audio_debug_start_monitor(void);
esp_err_t audio_debug_stop_monitor(void);
void audio_register_mic_level_callback(audio_mic_level_callback_t callback);
esp_err_t audio_debug_record_sample(uint8_t **data, int *len);
esp_err_t audio_debug_play_test_audio(void);

#ifdef __cplusplus
}
#endif
