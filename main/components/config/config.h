#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SSID_SIZE        32
#define WIFI_PASSWORD_SIZE    64
#define API_KEY_SIZE          128
#define BASE_URL_SIZE         128
#define MODEL_NAME_SIZE       64
#define VOICE_NAME_SIZE       64
#define APP_ID_SIZE           64
#define ACCESS_TOKEN_SIZE     256
#define SECRET_KEY_SIZE       128
#define PROXY_URL_SIZE        128
#define VOICE_GATEWAY_URL_SIZE 192
#define VOICE_TOKEN_SIZE      128

typedef enum {
    AI_PROVIDER_CONFIG_OPENAI = 0,
    AI_PROVIDER_CONFIG_ZHIPU,
    AI_PROVIDER_CONFIG_DEEPSEEK,
    AI_PROVIDER_CONFIG_MAX
} ai_provider_config_t;

typedef enum {
    VOICE_PROVIDER_CONFIG_VOLCENGINE = 0,
    VOICE_PROVIDER_CONFIG_ALIYUN,
    VOICE_PROVIDER_CONFIG_CUSTOM,
    VOICE_PROVIDER_CONFIG_MAX
} voice_provider_config_t;

typedef struct app_config_t {
    char wifi_ssid[WIFI_SSID_SIZE];
    char wifi_password[WIFI_PASSWORD_SIZE];

    /* Active chat provider selection */
    ai_provider_config_t provider;

    /* Active chat credentials (resolved from selected provider) */
    char api_key[API_KEY_SIZE];
    char base_url[BASE_URL_SIZE];
    char model_name[MODEL_NAME_SIZE];

    /* Provider-specific chat credentials */
    char openai_api_key[API_KEY_SIZE];
    char openai_base_url[BASE_URL_SIZE];
    char openai_model_name[MODEL_NAME_SIZE];

    char zhipu_api_key[API_KEY_SIZE];
    char zhipu_base_url[BASE_URL_SIZE];
    char zhipu_model_name[MODEL_NAME_SIZE];

    char deepseek_api_key[API_KEY_SIZE];
    char deepseek_base_url[BASE_URL_SIZE];
    char deepseek_model_name[MODEL_NAME_SIZE];

    /* STT provider config */
    voice_provider_config_t stt_provider;
    char stt_api_key[API_KEY_SIZE];
    char stt_app_id[APP_ID_SIZE];
    char stt_access_token[ACCESS_TOKEN_SIZE];
    char stt_secret_key[SECRET_KEY_SIZE];
    char stt_base_url[BASE_URL_SIZE];
    char stt_model_name[MODEL_NAME_SIZE];

    /* TTS provider config */
    voice_provider_config_t tts_provider;
    char tts_api_key[API_KEY_SIZE];
    char tts_app_id[APP_ID_SIZE];
    char tts_access_token[ACCESS_TOKEN_SIZE];
    char tts_secret_key[SECRET_KEY_SIZE];
    char tts_base_url[BASE_URL_SIZE];
    char tts_voice_name[VOICE_NAME_SIZE];

    /* Voice provider profiles */
    char voice_volcengine_app_id[APP_ID_SIZE];
    char voice_volcengine_access_token[ACCESS_TOKEN_SIZE];
    char voice_volcengine_secret_key[SECRET_KEY_SIZE];
    char voice_volcengine_stt_base_url[BASE_URL_SIZE];
    char voice_volcengine_stt_model[MODEL_NAME_SIZE];
    char voice_volcengine_tts_base_url[BASE_URL_SIZE];
    char voice_volcengine_tts_voice[VOICE_NAME_SIZE];

    char voice_aliyun_api_key[API_KEY_SIZE];
    char voice_aliyun_stt_base_url[BASE_URL_SIZE];
    char voice_aliyun_stt_model[MODEL_NAME_SIZE];
    char voice_aliyun_tts_base_url[BASE_URL_SIZE];
    char voice_aliyun_tts_voice[VOICE_NAME_SIZE];

    char voice_custom_api_key[API_KEY_SIZE];
    char voice_custom_stt_base_url[BASE_URL_SIZE];
    char voice_custom_stt_model[MODEL_NAME_SIZE];
    char voice_custom_tts_base_url[BASE_URL_SIZE];
    char voice_custom_tts_voice[VOICE_NAME_SIZE];

    /* Optional voice gateway */
    char voice_gateway_url[VOICE_GATEWAY_URL_SIZE];
    char voice_gateway_token[VOICE_TOKEN_SIZE];
    bool enable_voice_gateway;

    /* Voice runtime parameters */
    bool enable_barge_in;
    int vad_silence_ms;
    int stt_timeout_ms;
    int tts_timeout_ms;
    int audio_chunk_ms;
    
    char proxy_url[PROXY_URL_SIZE];
    bool enable_proxy;
    
    float temperature;
    int max_tokens;
    int history_limit;
    
    int volume;
    int sampling_rate;
} app_config_t;

esp_err_t config_init(void);
esp_err_t config_load_from_nvs(void);
esp_err_t config_save_to_nvs(void);
esp_err_t config_factory_reset(void);
app_config_t* config_get(void);
esp_err_t config_set_wifi(const char *ssid, const char *password);
esp_err_t config_set_ai_provider(ai_provider_config_t provider);
esp_err_t config_set_ai_credentials(const char *api_key, const char *base_url, const char *model_name);
esp_err_t config_set_ai_credentials_for_provider(ai_provider_config_t provider,
                                                 const char *api_key,
                                                 const char *base_url,
                                                 const char *model_name);

esp_err_t config_set_stt_provider(voice_provider_config_t provider);
esp_err_t config_set_tts_provider(voice_provider_config_t provider);
esp_err_t config_set_voice_provider_profile(voice_provider_config_t provider,
                                            const char *api_key,
                                            const char *stt_base_url,
                                            const char *tts_base_url);
esp_err_t config_set_stt_endpoint(const char *base_url, const char *model_name);
esp_err_t config_set_tts_endpoint(const char *base_url, const char *voice_name);
esp_err_t config_set_voice_gateway(const char *gateway_url, const char *gateway_token, bool enable);
esp_err_t config_set_voice_parameters(bool enable_barge_in,
                                      int vad_silence_ms,
                                      int stt_timeout_ms,
                                      int tts_timeout_ms,
                                      int audio_chunk_ms);

esp_err_t config_set_proxy(const char *proxy_url, bool enable);
esp_err_t config_set_parameters(float temperature, int max_tokens, int history_limit);

#ifdef __cplusplus
}
#endif
