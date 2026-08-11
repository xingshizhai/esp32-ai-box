#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct embedded_voice_gateway embedded_voice_gateway_t;

typedef struct {
    const char *api_key;
    const char *websocket_url;
    const char *stt_model;
    const char *tts_model;
    const char *tts_voice;
    int timeout_ms;
} embedded_voice_gateway_cfg_t;

embedded_voice_gateway_t *embedded_voice_gateway_create(const embedded_voice_gateway_cfg_t *cfg);
void embedded_voice_gateway_destroy(embedded_voice_gateway_t *gateway);

esp_err_t embedded_voice_gateway_stt_start(embedded_voice_gateway_t *gateway,
                                           const char *session_id,
                                           int sample_rate_hz);
esp_err_t embedded_voice_gateway_stt_send_audio(embedded_voice_gateway_t *gateway,
                                                const uint8_t *pcm,
                                                int len);
uint32_t embedded_voice_gateway_stt_result_revision(embedded_voice_gateway_t *gateway);
esp_err_t embedded_voice_gateway_stt_stop(embedded_voice_gateway_t *gateway,
                                          char *out_text,
                                          int out_text_size);
esp_err_t embedded_voice_gateway_tts(embedded_voice_gateway_t *gateway,
                                     const char *session_id,
                                     const char *text,
                                     const char *voice_name,
                                     uint8_t **audio_data,
                                     int *audio_len);
