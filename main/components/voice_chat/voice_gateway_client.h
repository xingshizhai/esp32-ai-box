#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *base_url;
    const char *access_token;
    int timeout_ms;

    /* Optional STT profile passed through to voice gateway. */
    const char *stt_provider;
    const char *stt_model_name;
    const char *stt_api_key;
    const char *stt_app_id;
    const char *stt_access_token;
    const char *stt_secret_key;
    const char *stt_base_url;
    const char *stt_language;
    bool stt_enable_itn;
    bool stt_enable_punc;
    bool stt_enable_nonstream;
    const char *stt_result_type;
    int stt_end_window_size_ms;

    /* Optional TTS profile passed through to voice gateway. */
    const char *tts_provider;
    const char *tts_model_name;
    const char *tts_api_key;
    const char *tts_app_id;
    const char *tts_access_token;
    const char *tts_secret_key;
    const char *tts_base_url;
    const char *tts_transport;
} voice_gateway_client_cfg_t;

typedef struct voice_gateway_client voice_gateway_client_t;

voice_gateway_client_t *voice_gateway_client_create(const voice_gateway_client_cfg_t *cfg);
void voice_gateway_client_destroy(voice_gateway_client_t *client);

esp_err_t voice_gateway_stt_start(voice_gateway_client_t *client,
                                  const char *session_id,
                                  int sample_rate_hz);
esp_err_t voice_gateway_stt_send_audio(voice_gateway_client_t *client,
                                       const char *session_id,
                                       const uint8_t *pcm,
                                       int len);
esp_err_t voice_gateway_stt_stop(voice_gateway_client_t *client,
                                 const char *session_id,
                                 char *out_text,
                                 int out_text_size);

esp_err_t voice_gateway_tts_synthesize(voice_gateway_client_t *client,
                                       const char *session_id,
                                       const char *text,
                                       const char *voice_name,
                                       uint8_t **audio_data,
                                       int *audio_len);

#ifdef __cplusplus
}
#endif
