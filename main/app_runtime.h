#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "network.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_runtime_init(bool ui_ready);
void app_runtime_process_requests(void);

void app_runtime_request_voice_round(void);
void app_runtime_request_debug_record(void);
void app_runtime_request_debug_play_record(void);
void app_runtime_request_debug_play(void);
void app_runtime_set_debug_play_volume(int volume);

void app_runtime_handle_network_state(net_state_t state, void *user_data);
void app_runtime_handle_stt_result(const char *text);
void app_runtime_handle_audio_playback_complete(void);
void app_runtime_handle_mic_level(int level);

#if CONFIG_SDCARD_ENABLED
void app_runtime_request_debug_sdcard(void);
void app_runtime_request_debug_test_audio(void);
#endif

#ifdef __cplusplus
}
#endif