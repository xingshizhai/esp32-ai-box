#include "config.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <stdint.h>
#include <string.h>

#ifndef CONFIG_OPENAI_API_KEY_DEFAULT
#define CONFIG_OPENAI_API_KEY_DEFAULT ""
#endif

#ifndef CONFIG_OPENAI_BASE_URL_DEFAULT
#define CONFIG_OPENAI_BASE_URL_DEFAULT "https://api.openai.com/v1/chat/completions"
#endif

#ifndef CONFIG_OPENAI_MODEL_DEFAULT
#define CONFIG_OPENAI_MODEL_DEFAULT "gpt-4o-mini"
#endif

#ifndef CONFIG_ZHIPU_API_KEY_DEFAULT
#define CONFIG_ZHIPU_API_KEY_DEFAULT ""
#endif

#ifndef CONFIG_ZHIPU_BASE_URL_DEFAULT
#define CONFIG_ZHIPU_BASE_URL_DEFAULT "https://open.bigmodel.cn/api/paas/v4/chat/completions"
#endif

#ifndef CONFIG_ZHIPU_MODEL_DEFAULT
#define CONFIG_ZHIPU_MODEL_DEFAULT "glm-4-flash"
#endif

#ifndef CONFIG_DEEPSEEK_API_KEY_DEFAULT
#define CONFIG_DEEPSEEK_API_KEY_DEFAULT ""
#endif

#ifndef CONFIG_DEEPSEEK_BASE_URL_DEFAULT
#define CONFIG_DEEPSEEK_BASE_URL_DEFAULT "https://api.deepseek.com/v1/chat/completions"
#endif

#ifndef CONFIG_DEEPSEEK_MODEL_DEFAULT
#define CONFIG_DEEPSEEK_MODEL_DEFAULT "deepseek-chat"
#endif

#ifndef CONFIG_VOICE_VOLCENGINE_APP_ID
#define CONFIG_VOICE_VOLCENGINE_APP_ID ""
#endif

#ifndef CONFIG_VOICE_VOLCENGINE_ACCESS_TOKEN
#define CONFIG_VOICE_VOLCENGINE_ACCESS_TOKEN ""
#endif

#ifndef CONFIG_VOICE_VOLCENGINE_SECRET_KEY
#define CONFIG_VOICE_VOLCENGINE_SECRET_KEY ""
#endif

#ifndef CONFIG_VOICE_VOLCENGINE_STT_BASE_URL
#define CONFIG_VOICE_VOLCENGINE_STT_BASE_URL ""
#endif

#ifndef CONFIG_VOICE_VOLCENGINE_STT_MODEL
#define CONFIG_VOICE_VOLCENGINE_STT_MODEL "bigmodel"
#endif

#ifndef CONFIG_VOICE_VOLCENGINE_TTS_BASE_URL
#define CONFIG_VOICE_VOLCENGINE_TTS_BASE_URL ""
#endif

#ifndef CONFIG_VOICE_VOLCENGINE_TTS_VOICE
#define CONFIG_VOICE_VOLCENGINE_TTS_VOICE ""
#endif

#ifndef CONFIG_VOICE_ALIYUN_API_KEY
#define CONFIG_VOICE_ALIYUN_API_KEY ""
#endif

#ifndef CONFIG_VOICE_ALIYUN_STT_BASE_URL
#define CONFIG_VOICE_ALIYUN_STT_BASE_URL ""
#endif

#ifndef CONFIG_VOICE_ALIYUN_STT_MODEL
#define CONFIG_VOICE_ALIYUN_STT_MODEL ""
#endif

#ifndef CONFIG_VOICE_ALIYUN_TTS_BASE_URL
#define CONFIG_VOICE_ALIYUN_TTS_BASE_URL ""
#endif

#ifndef CONFIG_VOICE_ALIYUN_TTS_VOICE
#define CONFIG_VOICE_ALIYUN_TTS_VOICE ""
#endif

#ifndef CONFIG_VOICE_CUSTOM_API_KEY
#define CONFIG_VOICE_CUSTOM_API_KEY ""
#endif

#ifndef CONFIG_VOICE_CUSTOM_STT_BASE_URL
#define CONFIG_VOICE_CUSTOM_STT_BASE_URL ""
#endif

#ifndef CONFIG_VOICE_CUSTOM_STT_MODEL
#define CONFIG_VOICE_CUSTOM_STT_MODEL ""
#endif

#ifndef CONFIG_VOICE_CUSTOM_TTS_BASE_URL
#define CONFIG_VOICE_CUSTOM_TTS_BASE_URL ""
#endif

#ifndef CONFIG_VOICE_CUSTOM_TTS_VOICE
#define CONFIG_VOICE_CUSTOM_TTS_VOICE ""
#endif

#define VOLCENGINE_STT_ASYNC_BASE_URL "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
#define VOLCENGINE_TTS_HTTP_BASE_URL "https://openspeech.bytedance.com/api/v1/tts"

#define NVS_NAMESPACE "ai_chat"
#define ESP_RETURN_ON_ERROR(x, tag, msg) do { \
    esp_err_t err_ = (x); \
    if (err_ != ESP_OK) { \
        ESP_LOGE(tag, "%s: %s", msg, esp_err_to_name(err_)); \
        return err_; \
    } \
} while (0)

static const char *TAG = "config";
static app_config_t s_config = {0};

typedef struct {
    char *value;
    size_t size;
} config_string_slot_t;

typedef struct {
    config_string_slot_t api_key;
    config_string_slot_t base_url;
    config_string_slot_t model_name;
} config_chat_profile_slots_t;

typedef struct {
    config_string_slot_t api_key;
    config_string_slot_t app_id;
    config_string_slot_t access_token;
    config_string_slot_t secret_key;
    config_string_slot_t stt_base_url;
    config_string_slot_t stt_model;
    config_string_slot_t tts_base_url;
    config_string_slot_t tts_voice;
} config_voice_profile_slots_t;

static void config_copy_string(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static void config_string_slot_set(config_string_slot_t slot, const char *src)
{
    if (slot.value == NULL || slot.size == 0) {
        return;
    }

    config_copy_string(slot.value, slot.size, src);
}

static void config_string_slot_clear(config_string_slot_t slot)
{
    if (slot.value == NULL || slot.size == 0) {
        return;
    }

    slot.value[0] = '\0';
}

static bool config_get_chat_profile_slots(ai_provider_config_t provider,
                                          config_chat_profile_slots_t *slots)
{
    if (slots == NULL) {
        return false;
    }

    memset(slots, 0, sizeof(*slots));

    switch (provider) {
        case AI_PROVIDER_CONFIG_OPENAI:
            slots->api_key = (config_string_slot_t){s_config.openai_api_key, sizeof(s_config.openai_api_key)};
            slots->base_url = (config_string_slot_t){s_config.openai_base_url, sizeof(s_config.openai_base_url)};
            slots->model_name = (config_string_slot_t){s_config.openai_model_name, sizeof(s_config.openai_model_name)};
            return true;
        case AI_PROVIDER_CONFIG_ZHIPU:
            slots->api_key = (config_string_slot_t){s_config.zhipu_api_key, sizeof(s_config.zhipu_api_key)};
            slots->base_url = (config_string_slot_t){s_config.zhipu_base_url, sizeof(s_config.zhipu_base_url)};
            slots->model_name = (config_string_slot_t){s_config.zhipu_model_name, sizeof(s_config.zhipu_model_name)};
            return true;
        case AI_PROVIDER_CONFIG_DEEPSEEK:
            slots->api_key = (config_string_slot_t){s_config.deepseek_api_key, sizeof(s_config.deepseek_api_key)};
            slots->base_url = (config_string_slot_t){s_config.deepseek_base_url, sizeof(s_config.deepseek_base_url)};
            slots->model_name = (config_string_slot_t){s_config.deepseek_model_name, sizeof(s_config.deepseek_model_name)};
            return true;
        default:
            return false;
    }
}

static bool config_get_voice_profile_slots(voice_provider_config_t provider,
                                           config_voice_profile_slots_t *slots)
{
    if (slots == NULL) {
        return false;
    }

    memset(slots, 0, sizeof(*slots));

    switch (provider) {
        case VOICE_PROVIDER_CONFIG_VOLCENGINE:
            slots->api_key = (config_string_slot_t){s_config.voice_volcengine_access_token,
                                                    sizeof(s_config.voice_volcengine_access_token)};
            slots->app_id = (config_string_slot_t){s_config.voice_volcengine_app_id,
                                                   sizeof(s_config.voice_volcengine_app_id)};
            slots->access_token = (config_string_slot_t){s_config.voice_volcengine_access_token,
                                                         sizeof(s_config.voice_volcengine_access_token)};
            slots->secret_key = (config_string_slot_t){s_config.voice_volcengine_secret_key,
                                                       sizeof(s_config.voice_volcengine_secret_key)};
            slots->stt_base_url = (config_string_slot_t){s_config.voice_volcengine_stt_base_url,
                                                         sizeof(s_config.voice_volcengine_stt_base_url)};
            slots->stt_model = (config_string_slot_t){s_config.voice_volcengine_stt_model,
                                                      sizeof(s_config.voice_volcengine_stt_model)};
            slots->tts_base_url = (config_string_slot_t){s_config.voice_volcengine_tts_base_url,
                                                         sizeof(s_config.voice_volcengine_tts_base_url)};
            slots->tts_voice = (config_string_slot_t){s_config.voice_volcengine_tts_voice,
                                                      sizeof(s_config.voice_volcengine_tts_voice)};
            return true;
        case VOICE_PROVIDER_CONFIG_ALIYUN:
            slots->api_key = (config_string_slot_t){s_config.voice_aliyun_api_key,
                                                    sizeof(s_config.voice_aliyun_api_key)};
            slots->stt_base_url = (config_string_slot_t){s_config.voice_aliyun_stt_base_url,
                                                         sizeof(s_config.voice_aliyun_stt_base_url)};
            slots->stt_model = (config_string_slot_t){s_config.voice_aliyun_stt_model,
                                                      sizeof(s_config.voice_aliyun_stt_model)};
            slots->tts_base_url = (config_string_slot_t){s_config.voice_aliyun_tts_base_url,
                                                         sizeof(s_config.voice_aliyun_tts_base_url)};
            slots->tts_voice = (config_string_slot_t){s_config.voice_aliyun_tts_voice,
                                                      sizeof(s_config.voice_aliyun_tts_voice)};
            return true;
        case VOICE_PROVIDER_CONFIG_CUSTOM:
            slots->api_key = (config_string_slot_t){s_config.voice_custom_api_key,
                                                    sizeof(s_config.voice_custom_api_key)};
            slots->stt_base_url = (config_string_slot_t){s_config.voice_custom_stt_base_url,
                                                         sizeof(s_config.voice_custom_stt_base_url)};
            slots->stt_model = (config_string_slot_t){s_config.voice_custom_stt_model,
                                                      sizeof(s_config.voice_custom_stt_model)};
            slots->tts_base_url = (config_string_slot_t){s_config.voice_custom_tts_base_url,
                                                         sizeof(s_config.voice_custom_tts_base_url)};
            slots->tts_voice = (config_string_slot_t){s_config.voice_custom_tts_voice,
                                                      sizeof(s_config.voice_custom_tts_voice)};
            return true;
        default:
            return false;
    }
}

static ai_provider_config_t config_default_provider(void)
{
#if CONFIG_AI_PROVIDER_OPENAI
    return AI_PROVIDER_CONFIG_OPENAI;
#elif CONFIG_AI_PROVIDER_ZHIPU
    return AI_PROVIDER_CONFIG_ZHIPU;
#else
    return AI_PROVIDER_CONFIG_DEEPSEEK;
#endif
}

static voice_provider_config_t config_default_stt_provider(void)
{
#if CONFIG_STT_PROVIDER_ALIYUN
    return VOICE_PROVIDER_CONFIG_ALIYUN;
#elif CONFIG_STT_PROVIDER_CUSTOM
    return VOICE_PROVIDER_CONFIG_CUSTOM;
#else
    return VOICE_PROVIDER_CONFIG_VOLCENGINE;
#endif
}

static voice_provider_config_t config_default_tts_provider(void)
{
#if CONFIG_TTS_PROVIDER_ALIYUN
    return VOICE_PROVIDER_CONFIG_ALIYUN;
#elif CONFIG_TTS_PROVIDER_CUSTOM
    return VOICE_PROVIDER_CONFIG_CUSTOM;
#else
    return VOICE_PROVIDER_CONFIG_VOLCENGINE;
#endif
}

static void config_reset_voice_provider_defaults(void)
{
    config_copy_string(s_config.voice_volcengine_app_id,
                       sizeof(s_config.voice_volcengine_app_id),
                       CONFIG_VOICE_VOLCENGINE_APP_ID);
    config_copy_string(s_config.voice_volcengine_access_token,
                       sizeof(s_config.voice_volcengine_access_token),
                       CONFIG_VOICE_VOLCENGINE_ACCESS_TOKEN);
    config_copy_string(s_config.voice_volcengine_secret_key,
                       sizeof(s_config.voice_volcengine_secret_key),
                       CONFIG_VOICE_VOLCENGINE_SECRET_KEY);
    config_copy_string(s_config.voice_volcengine_stt_base_url,
                       sizeof(s_config.voice_volcengine_stt_base_url),
                       CONFIG_VOICE_VOLCENGINE_STT_BASE_URL);
    config_copy_string(s_config.voice_volcengine_stt_model,
                       sizeof(s_config.voice_volcengine_stt_model),
                       CONFIG_VOICE_VOLCENGINE_STT_MODEL);
    config_copy_string(s_config.voice_volcengine_tts_base_url,
                       sizeof(s_config.voice_volcengine_tts_base_url),
                       CONFIG_VOICE_VOLCENGINE_TTS_BASE_URL);
    config_copy_string(s_config.voice_volcengine_tts_voice,
                       sizeof(s_config.voice_volcengine_tts_voice),
                       CONFIG_VOICE_VOLCENGINE_TTS_VOICE);

    if (s_config.voice_volcengine_stt_base_url[0] == '\0') {
        config_copy_string(s_config.voice_volcengine_stt_base_url,
                           sizeof(s_config.voice_volcengine_stt_base_url),
                           VOLCENGINE_STT_ASYNC_BASE_URL);
    }
    if (s_config.voice_volcengine_tts_base_url[0] == '\0') {
        config_copy_string(s_config.voice_volcengine_tts_base_url,
                           sizeof(s_config.voice_volcengine_tts_base_url),
                           VOLCENGINE_TTS_HTTP_BASE_URL);
    }

    config_copy_string(s_config.voice_aliyun_api_key,
                       sizeof(s_config.voice_aliyun_api_key),
                       CONFIG_VOICE_ALIYUN_API_KEY);
    config_copy_string(s_config.voice_aliyun_stt_base_url,
                       sizeof(s_config.voice_aliyun_stt_base_url),
                       CONFIG_VOICE_ALIYUN_STT_BASE_URL);
    config_copy_string(s_config.voice_aliyun_stt_model,
                       sizeof(s_config.voice_aliyun_stt_model),
                       CONFIG_VOICE_ALIYUN_STT_MODEL);
    config_copy_string(s_config.voice_aliyun_tts_base_url,
                       sizeof(s_config.voice_aliyun_tts_base_url),
                       CONFIG_VOICE_ALIYUN_TTS_BASE_URL);
    config_copy_string(s_config.voice_aliyun_tts_voice,
                       sizeof(s_config.voice_aliyun_tts_voice),
                       CONFIG_VOICE_ALIYUN_TTS_VOICE);

    config_copy_string(s_config.voice_custom_api_key,
                       sizeof(s_config.voice_custom_api_key),
                       CONFIG_VOICE_CUSTOM_API_KEY);
    config_copy_string(s_config.voice_custom_stt_base_url,
                       sizeof(s_config.voice_custom_stt_base_url),
                       CONFIG_VOICE_CUSTOM_STT_BASE_URL);
    config_copy_string(s_config.voice_custom_stt_model,
                       sizeof(s_config.voice_custom_stt_model),
                       CONFIG_VOICE_CUSTOM_STT_MODEL);
    config_copy_string(s_config.voice_custom_tts_base_url,
                       sizeof(s_config.voice_custom_tts_base_url),
                       CONFIG_VOICE_CUSTOM_TTS_BASE_URL);
    config_copy_string(s_config.voice_custom_tts_voice,
                       sizeof(s_config.voice_custom_tts_voice),
                       CONFIG_VOICE_CUSTOM_TTS_VOICE);
}

static void config_apply_active_voice_profiles(void)
{
    config_voice_profile_slots_t stt_profile = {0};
    config_voice_profile_slots_t tts_profile = {0};

    config_string_slot_t stt_api_key = {s_config.stt_api_key, sizeof(s_config.stt_api_key)};
    config_string_slot_t stt_app_id = {s_config.stt_app_id, sizeof(s_config.stt_app_id)};
    config_string_slot_t stt_access_token = {s_config.stt_access_token, sizeof(s_config.stt_access_token)};
    config_string_slot_t stt_secret_key = {s_config.stt_secret_key, sizeof(s_config.stt_secret_key)};
    config_string_slot_t stt_base_url = {s_config.stt_base_url, sizeof(s_config.stt_base_url)};
    config_string_slot_t stt_model_name = {s_config.stt_model_name, sizeof(s_config.stt_model_name)};

    config_string_slot_t tts_api_key = {s_config.tts_api_key, sizeof(s_config.tts_api_key)};
    config_string_slot_t tts_app_id = {s_config.tts_app_id, sizeof(s_config.tts_app_id)};
    config_string_slot_t tts_access_token = {s_config.tts_access_token, sizeof(s_config.tts_access_token)};
    config_string_slot_t tts_secret_key = {s_config.tts_secret_key, sizeof(s_config.tts_secret_key)};
    config_string_slot_t tts_base_url = {s_config.tts_base_url, sizeof(s_config.tts_base_url)};
    config_string_slot_t tts_voice_name = {s_config.tts_voice_name, sizeof(s_config.tts_voice_name)};

    config_string_slot_clear(stt_api_key);
    config_string_slot_clear(stt_app_id);
    config_string_slot_clear(stt_access_token);
    config_string_slot_clear(stt_secret_key);
    config_string_slot_clear(stt_base_url);
    config_string_slot_clear(stt_model_name);

    if (config_get_voice_profile_slots(s_config.stt_provider, &stt_profile)) {
        config_string_slot_set(stt_api_key, stt_profile.api_key.value);
        config_string_slot_set(stt_app_id, stt_profile.app_id.value);
        config_string_slot_set(stt_access_token, stt_profile.access_token.value);
        config_string_slot_set(stt_secret_key, stt_profile.secret_key.value);
        config_string_slot_set(stt_base_url, stt_profile.stt_base_url.value);
        config_string_slot_set(stt_model_name, stt_profile.stt_model.value);
    }

    config_string_slot_clear(tts_api_key);
    config_string_slot_clear(tts_app_id);
    config_string_slot_clear(tts_access_token);
    config_string_slot_clear(tts_secret_key);
    config_string_slot_clear(tts_base_url);
    config_string_slot_clear(tts_voice_name);

    if (config_get_voice_profile_slots(s_config.tts_provider, &tts_profile)) {
        config_string_slot_set(tts_api_key, tts_profile.api_key.value);
        config_string_slot_set(tts_app_id, tts_profile.app_id.value);
        config_string_slot_set(tts_access_token, tts_profile.access_token.value);
        config_string_slot_set(tts_secret_key, tts_profile.secret_key.value);
        config_string_slot_set(tts_base_url, tts_profile.tts_base_url.value);
        config_string_slot_set(tts_voice_name, tts_profile.tts_voice.value);
    }
}

static esp_err_t config_update_voice_provider_stt_model(voice_provider_config_t provider,
                                                        const char *model_name)
{
    config_voice_profile_slots_t profile = {0};

    if (model_name == NULL || !config_get_voice_profile_slots(provider, &profile)) {
        return ESP_ERR_INVALID_ARG;
    }

    config_string_slot_set(profile.stt_model, model_name);
    return ESP_OK;
}

static esp_err_t config_update_voice_provider_tts_voice(voice_provider_config_t provider,
                                                        const char *voice_name)
{
    config_voice_profile_slots_t profile = {0};

    if (voice_name == NULL || !config_get_voice_profile_slots(provider, &profile)) {
        return ESP_ERR_INVALID_ARG;
    }

    config_string_slot_set(profile.tts_voice, voice_name);
    return ESP_OK;
}

static esp_err_t config_update_voice_provider_profile(voice_provider_config_t provider,
                                                      const char *api_key,
                                                      const char *stt_base_url,
                                                      const char *tts_base_url)
{
    config_voice_profile_slots_t profile = {0};

    if (!config_get_voice_profile_slots(provider, &profile)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (api_key != NULL) {
        config_string_slot_set(profile.api_key, api_key);
    }
    if (stt_base_url != NULL) {
        config_string_slot_set(profile.stt_base_url, stt_base_url);
    }
    if (tts_base_url != NULL) {
        config_string_slot_set(profile.tts_base_url, tts_base_url);
    }

    return ESP_OK;
}

static void config_apply_active_chat_profile(void)
{
    config_chat_profile_slots_t profile = {0};

    if (!config_get_chat_profile_slots(s_config.provider, &profile)) {
        s_config.api_key[0] = '\0';
        s_config.base_url[0] = '\0';
        s_config.model_name[0] = '\0';
        return;
    }

    config_copy_string(s_config.api_key, sizeof(s_config.api_key), profile.api_key.value);
    config_copy_string(s_config.base_url, sizeof(s_config.base_url), profile.base_url.value);
    config_copy_string(s_config.model_name, sizeof(s_config.model_name), profile.model_name.value);
}

static void config_reset_chat_profile_defaults(void)
{
    config_copy_string(s_config.openai_api_key, sizeof(s_config.openai_api_key), CONFIG_OPENAI_API_KEY_DEFAULT);
    config_copy_string(s_config.openai_base_url, sizeof(s_config.openai_base_url), CONFIG_OPENAI_BASE_URL_DEFAULT);
    config_copy_string(s_config.openai_model_name, sizeof(s_config.openai_model_name), CONFIG_OPENAI_MODEL_DEFAULT);

    config_copy_string(s_config.zhipu_api_key, sizeof(s_config.zhipu_api_key), CONFIG_ZHIPU_API_KEY_DEFAULT);
    config_copy_string(s_config.zhipu_base_url, sizeof(s_config.zhipu_base_url), CONFIG_ZHIPU_BASE_URL_DEFAULT);
    config_copy_string(s_config.zhipu_model_name, sizeof(s_config.zhipu_model_name), CONFIG_ZHIPU_MODEL_DEFAULT);

    config_copy_string(s_config.deepseek_api_key, sizeof(s_config.deepseek_api_key), CONFIG_DEEPSEEK_API_KEY_DEFAULT);
    config_copy_string(s_config.deepseek_base_url, sizeof(s_config.deepseek_base_url), CONFIG_DEEPSEEK_BASE_URL_DEFAULT);
    config_copy_string(s_config.deepseek_model_name, sizeof(s_config.deepseek_model_name), CONFIG_DEEPSEEK_MODEL_DEFAULT);
}

static esp_err_t config_update_provider_profile(ai_provider_config_t provider,
                                                const char *api_key,
                                                const char *base_url,
                                                const char *model_name)
{
    config_chat_profile_slots_t profile = {0};

    if (!config_get_chat_profile_slots(provider, &profile)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (api_key != NULL) {
        config_string_slot_set(profile.api_key, api_key);
    }
    if (base_url != NULL) {
        config_string_slot_set(profile.base_url, base_url);
    }
    if (model_name != NULL) {
        config_string_slot_set(profile.model_name, model_name);
    }

    return ESP_OK;
}

static void config_reset_to_defaults(void)
{
    memset(&s_config, 0, sizeof(s_config));

    config_copy_string(s_config.wifi_ssid, sizeof(s_config.wifi_ssid), CONFIG_WIFI_SSID);
    config_copy_string(s_config.wifi_password, sizeof(s_config.wifi_password), CONFIG_WIFI_PASSWORD);

    s_config.provider = config_default_provider();
    config_reset_chat_profile_defaults();
    config_apply_active_chat_profile();

    s_config.stt_provider = config_default_stt_provider();
    s_config.tts_provider = config_default_tts_provider();

    config_reset_voice_provider_defaults();
    config_apply_active_voice_profiles();

    config_copy_string(s_config.voice_gateway_url, sizeof(s_config.voice_gateway_url), CONFIG_DEFAULT_VOICE_GATEWAY_URL);
    config_copy_string(s_config.voice_gateway_token, sizeof(s_config.voice_gateway_token), CONFIG_DEFAULT_VOICE_GATEWAY_TOKEN);
    s_config.enable_voice_gateway = CONFIG_ENABLE_VOICE_GATEWAY;

    s_config.enable_barge_in = CONFIG_ENABLE_BARGE_IN;
    s_config.vad_silence_ms = CONFIG_VAD_SILENCE_MS;
    s_config.stt_timeout_ms = CONFIG_STT_TIMEOUT_MS;
    s_config.tts_timeout_ms = CONFIG_TTS_TIMEOUT_MS;
    s_config.audio_chunk_ms = CONFIG_AUDIO_CHUNK_MS;

    s_config.enable_proxy = false;
    s_config.proxy_url[0] = '\0';

    s_config.temperature = CONFIG_TEMPERATURE;
    s_config.max_tokens = CONFIG_MAX_TOKENS;
    s_config.history_limit = CONFIG_HISTORY_LIMIT;

    s_config.volume = CONFIG_VOLUM;
    s_config.sampling_rate = 16000;
}

static bool config_nvs_get_str(nvs_handle_t nvs_handle, const char *key, char *value, size_t value_size)
{
    size_t required_size = value_size;
    return nvs_get_str(nvs_handle, key, value, &required_size) == ESP_OK;
}

static bool config_nvs_get_u8(nvs_handle_t nvs_handle, const char *key, uint8_t *value)
{
    return nvs_get_u8(nvs_handle, key, value) == ESP_OK;
}

static bool config_nvs_get_i32(nvs_handle_t nvs_handle, const char *key, int32_t *value)
{
    return nvs_get_i32(nvs_handle, key, value) == ESP_OK;
}

esp_err_t config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS init failed");

    config_reset_to_defaults();
    return config_load_from_nvs();
}

esp_err_t config_load_from_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved config found, using defaults");
        return ESP_OK;
    }

    char legacy_api_key[API_KEY_SIZE] = {0};
    char legacy_base_url[BASE_URL_SIZE] = {0};
    char legacy_model_name[MODEL_NAME_SIZE] = {0};
    char legacy_stt_api_key[API_KEY_SIZE] = {0};
    char legacy_stt_base_url[BASE_URL_SIZE] = {0};
    char legacy_tts_api_key[API_KEY_SIZE] = {0};
    char legacy_tts_base_url[BASE_URL_SIZE] = {0};
    char legacy_stt_model_name[MODEL_NAME_SIZE] = {0};
    char legacy_tts_voice_name[VOICE_NAME_SIZE] = {0};
    char legacy_volcengine_api_key[API_KEY_SIZE] = {0};
    bool has_legacy_api_key = false;
    bool has_legacy_base_url = false;
    bool has_legacy_model_name = false;
    bool has_legacy_stt_api_key = false;
    bool has_legacy_stt_base_url = false;
    bool has_legacy_tts_api_key = false;
    bool has_legacy_tts_base_url = false;
    bool has_legacy_stt_model_name = false;
    bool has_legacy_tts_voice_name = false;
    bool has_legacy_volcengine_api_key = false;

    (void)config_nvs_get_str(nvs_handle, "wifi_ssid", s_config.wifi_ssid, sizeof(s_config.wifi_ssid));
    (void)config_nvs_get_str(nvs_handle, "wifi_password", s_config.wifi_password, sizeof(s_config.wifi_password));

    uint8_t u8 = 0;
    if (config_nvs_get_u8(nvs_handle, "provider", &u8) && u8 < AI_PROVIDER_CONFIG_MAX) {
        s_config.provider = (ai_provider_config_t)u8;
    }
    if (config_nvs_get_u8(nvs_handle, "stt_provider", &u8) && u8 < VOICE_PROVIDER_CONFIG_MAX) {
        s_config.stt_provider = (voice_provider_config_t)u8;
    }
    if (config_nvs_get_u8(nvs_handle, "tts_provider", &u8) && u8 < VOICE_PROVIDER_CONFIG_MAX) {
        s_config.tts_provider = (voice_provider_config_t)u8;
    }

    has_legacy_volcengine_api_key = config_nvs_get_str(nvs_handle,
                                                       "vce_key",
                                                       legacy_volcengine_api_key,
                                                       sizeof(legacy_volcengine_api_key));
    (void)config_nvs_get_str(nvs_handle,
                             "vce_appid",
                             s_config.voice_volcengine_app_id,
                             sizeof(s_config.voice_volcengine_app_id));
    (void)config_nvs_get_str(nvs_handle,
                             "vce_atok",
                             s_config.voice_volcengine_access_token,
                             sizeof(s_config.voice_volcengine_access_token));
    (void)config_nvs_get_str(nvs_handle,
                             "vce_skey",
                             s_config.voice_volcengine_secret_key,
                             sizeof(s_config.voice_volcengine_secret_key));
    (void)config_nvs_get_str(nvs_handle,
                             "vce_stt_url",
                             s_config.voice_volcengine_stt_base_url,
                             sizeof(s_config.voice_volcengine_stt_base_url));
    (void)config_nvs_get_str(nvs_handle,
                             "vce_stt_model",
                             s_config.voice_volcengine_stt_model,
                             sizeof(s_config.voice_volcengine_stt_model));
    (void)config_nvs_get_str(nvs_handle,
                             "vce_tts_url",
                             s_config.voice_volcengine_tts_base_url,
                             sizeof(s_config.voice_volcengine_tts_base_url));
    (void)config_nvs_get_str(nvs_handle,
                             "vce_tts_voice",
                             s_config.voice_volcengine_tts_voice,
                             sizeof(s_config.voice_volcengine_tts_voice));

    (void)config_nvs_get_str(nvs_handle,
                             "aly_key",
                             s_config.voice_aliyun_api_key,
                             sizeof(s_config.voice_aliyun_api_key));
    (void)config_nvs_get_str(nvs_handle,
                             "aly_stt_url",
                             s_config.voice_aliyun_stt_base_url,
                             sizeof(s_config.voice_aliyun_stt_base_url));
    (void)config_nvs_get_str(nvs_handle,
                             "aly_stt_model",
                             s_config.voice_aliyun_stt_model,
                             sizeof(s_config.voice_aliyun_stt_model));
    (void)config_nvs_get_str(nvs_handle,
                             "aly_tts_url",
                             s_config.voice_aliyun_tts_base_url,
                             sizeof(s_config.voice_aliyun_tts_base_url));
    (void)config_nvs_get_str(nvs_handle,
                             "aly_tts_voice",
                             s_config.voice_aliyun_tts_voice,
                             sizeof(s_config.voice_aliyun_tts_voice));

    (void)config_nvs_get_str(nvs_handle,
                             "cus_key",
                             s_config.voice_custom_api_key,
                             sizeof(s_config.voice_custom_api_key));
    (void)config_nvs_get_str(nvs_handle,
                             "cus_stt_url",
                             s_config.voice_custom_stt_base_url,
                             sizeof(s_config.voice_custom_stt_base_url));
    (void)config_nvs_get_str(nvs_handle,
                             "cus_stt_model",
                             s_config.voice_custom_stt_model,
                             sizeof(s_config.voice_custom_stt_model));
    (void)config_nvs_get_str(nvs_handle,
                             "cus_tts_url",
                             s_config.voice_custom_tts_base_url,
                             sizeof(s_config.voice_custom_tts_base_url));
    (void)config_nvs_get_str(nvs_handle,
                             "cus_tts_voice",
                             s_config.voice_custom_tts_voice,
                             sizeof(s_config.voice_custom_tts_voice));

    (void)config_nvs_get_str(nvs_handle, "oa_key", s_config.openai_api_key, sizeof(s_config.openai_api_key));
    (void)config_nvs_get_str(nvs_handle, "oa_url", s_config.openai_base_url, sizeof(s_config.openai_base_url));
    (void)config_nvs_get_str(nvs_handle, "oa_model", s_config.openai_model_name, sizeof(s_config.openai_model_name));

    (void)config_nvs_get_str(nvs_handle, "zp_key", s_config.zhipu_api_key, sizeof(s_config.zhipu_api_key));
    (void)config_nvs_get_str(nvs_handle, "zp_url", s_config.zhipu_base_url, sizeof(s_config.zhipu_base_url));
    (void)config_nvs_get_str(nvs_handle, "zp_model", s_config.zhipu_model_name, sizeof(s_config.zhipu_model_name));

    (void)config_nvs_get_str(nvs_handle, "ds_key", s_config.deepseek_api_key, sizeof(s_config.deepseek_api_key));
    (void)config_nvs_get_str(nvs_handle, "ds_url", s_config.deepseek_base_url, sizeof(s_config.deepseek_base_url));
    (void)config_nvs_get_str(nvs_handle, "ds_model", s_config.deepseek_model_name, sizeof(s_config.deepseek_model_name));

    has_legacy_api_key = config_nvs_get_str(nvs_handle, "api_key", legacy_api_key, sizeof(legacy_api_key));
    has_legacy_base_url = config_nvs_get_str(nvs_handle, "base_url", legacy_base_url, sizeof(legacy_base_url));
    has_legacy_model_name = config_nvs_get_str(nvs_handle, "model_name", legacy_model_name, sizeof(legacy_model_name));
    has_legacy_stt_api_key = config_nvs_get_str(nvs_handle,
                                                "stt_api_key",
                                                legacy_stt_api_key,
                                                sizeof(legacy_stt_api_key));
    has_legacy_stt_base_url = config_nvs_get_str(nvs_handle,
                                                 "stt_base_url",
                                                 legacy_stt_base_url,
                                                 sizeof(legacy_stt_base_url));
    has_legacy_tts_api_key = config_nvs_get_str(nvs_handle,
                                                "tts_api_key",
                                                legacy_tts_api_key,
                                                sizeof(legacy_tts_api_key));
    has_legacy_tts_base_url = config_nvs_get_str(nvs_handle,
                                                 "tts_base_url",
                                                 legacy_tts_base_url,
                                                 sizeof(legacy_tts_base_url));

    if (has_legacy_api_key || has_legacy_base_url || has_legacy_model_name) {
        config_chat_profile_slots_t chat_profile = {0};

        if (config_get_chat_profile_slots(s_config.provider, &chat_profile)) {
            if (has_legacy_api_key && chat_profile.api_key.value[0] == '\0') {
                config_string_slot_set(chat_profile.api_key, legacy_api_key);
            }
            if (has_legacy_base_url && chat_profile.base_url.value[0] == '\0') {
                config_string_slot_set(chat_profile.base_url, legacy_base_url);
            }
            if (has_legacy_model_name && chat_profile.model_name.value[0] == '\0') {
                config_string_slot_set(chat_profile.model_name, legacy_model_name);
            }
        }
    }

    if (has_legacy_stt_api_key || has_legacy_stt_base_url) {
        config_voice_profile_slots_t stt_profile = {0};

        if (config_get_voice_profile_slots(s_config.stt_provider, &stt_profile)) {
            if (has_legacy_stt_api_key && stt_profile.api_key.value != NULL && stt_profile.api_key.value[0] == '\0') {
                config_string_slot_set(stt_profile.api_key, legacy_stt_api_key);
            }
            if (has_legacy_stt_base_url && stt_profile.stt_base_url.value != NULL && stt_profile.stt_base_url.value[0] == '\0') {
                config_string_slot_set(stt_profile.stt_base_url, legacy_stt_base_url);
            }
        }
    }

    if (has_legacy_tts_api_key || has_legacy_tts_base_url) {
        config_voice_profile_slots_t tts_profile = {0};

        if (config_get_voice_profile_slots(s_config.tts_provider, &tts_profile)) {
            if (has_legacy_tts_api_key && tts_profile.api_key.value != NULL && tts_profile.api_key.value[0] == '\0') {
                config_string_slot_set(tts_profile.api_key, legacy_tts_api_key);
            }
            if (has_legacy_tts_base_url && tts_profile.tts_base_url.value != NULL && tts_profile.tts_base_url.value[0] == '\0') {
                config_string_slot_set(tts_profile.tts_base_url, legacy_tts_base_url);
            }
        }
    }

    has_legacy_stt_model_name = config_nvs_get_str(nvs_handle,
                                                   "stt_model_name",
                                                   legacy_stt_model_name,
                                                   sizeof(legacy_stt_model_name));
    has_legacy_tts_voice_name = config_nvs_get_str(nvs_handle,
                                                   "tts_voice_name",
                                                   legacy_tts_voice_name,
                                                   sizeof(legacy_tts_voice_name));

    (void)config_nvs_get_str(nvs_handle, "voice_gateway_url", s_config.voice_gateway_url, sizeof(s_config.voice_gateway_url));
    (void)config_nvs_get_str(nvs_handle, "voice_gateway_token", s_config.voice_gateway_token, sizeof(s_config.voice_gateway_token));

    if (config_nvs_get_u8(nvs_handle, "enable_voice_gateway", &u8)) {
        s_config.enable_voice_gateway = (u8 != 0);
    }
    if (config_nvs_get_u8(nvs_handle, "enable_barge_in", &u8)) {
        s_config.enable_barge_in = (u8 != 0);
    }

    (void)config_nvs_get_str(nvs_handle, "proxy_url", s_config.proxy_url, sizeof(s_config.proxy_url));
    if (config_nvs_get_u8(nvs_handle, "enable_proxy", &u8)) {
        s_config.enable_proxy = (u8 != 0);
    }

    int32_t i32 = 0;
    if (config_nvs_get_i32(nvs_handle, "temperature_milli", &i32)) {
        s_config.temperature = ((float)i32) / 1000.0f;
    }
    if (config_nvs_get_i32(nvs_handle, "max_tokens", &i32)) {
        s_config.max_tokens = i32;
    }
    if (config_nvs_get_i32(nvs_handle, "history_limit", &i32)) {
        s_config.history_limit = i32;
    }
    if (config_nvs_get_i32(nvs_handle, "volume", &i32)) {
        s_config.volume = i32;
    }
    if (config_nvs_get_i32(nvs_handle, "sampling_rate", &i32)) {
        s_config.sampling_rate = i32;
    }
    if (config_nvs_get_i32(nvs_handle, "vad_silence_ms", &i32)) {
        s_config.vad_silence_ms = i32;
    }
    if (config_nvs_get_i32(nvs_handle, "stt_timeout_ms", &i32)) {
        s_config.stt_timeout_ms = i32;
    }
    if (config_nvs_get_i32(nvs_handle, "tts_timeout_ms", &i32)) {
        s_config.tts_timeout_ms = i32;
    }
    if (config_nvs_get_i32(nvs_handle, "audio_chunk_ms", &i32)) {
        s_config.audio_chunk_ms = i32;
    }

    nvs_close(nvs_handle);

    if (strlen(s_config.wifi_ssid) == 0) {
        config_copy_string(s_config.wifi_ssid, sizeof(s_config.wifi_ssid), CONFIG_WIFI_SSID);
    }
    if (strlen(s_config.wifi_password) == 0) {
        config_copy_string(s_config.wifi_password, sizeof(s_config.wifi_password), CONFIG_WIFI_PASSWORD);
    }

    if (s_config.voice_volcengine_stt_base_url[0] == '\0') {
        config_copy_string(s_config.voice_volcengine_stt_base_url,
                           sizeof(s_config.voice_volcengine_stt_base_url),
                           VOLCENGINE_STT_ASYNC_BASE_URL);
    }
    if (s_config.voice_volcengine_tts_base_url[0] == '\0') {
        config_copy_string(s_config.voice_volcengine_tts_base_url,
                           sizeof(s_config.voice_volcengine_tts_base_url),
                           VOLCENGINE_TTS_HTTP_BASE_URL);
    }

    if (s_config.voice_volcengine_access_token[0] == '\0' && has_legacy_volcengine_api_key) {
        config_copy_string(s_config.voice_volcengine_access_token,
                           sizeof(s_config.voice_volcengine_access_token),
                           legacy_volcengine_api_key);
    }

    if (strcmp(s_config.voice_volcengine_stt_model, "bigmodel_async") == 0) {
        config_copy_string(s_config.voice_volcengine_stt_model,
                           sizeof(s_config.voice_volcengine_stt_model),
                           "bigmodel");
    }

    if (has_legacy_stt_model_name) {
        config_voice_profile_slots_t stt_profile = {0};
        const char *legacy_stt_model_value = legacy_stt_model_name;

        if (strcmp(legacy_stt_model_name, "bigmodel_async") == 0) {
            legacy_stt_model_value = "bigmodel";
        }

        if (config_get_voice_profile_slots(s_config.stt_provider, &stt_profile) &&
            stt_profile.stt_model.value != NULL &&
            stt_profile.stt_model.value[0] == '\0') {
            (void)config_update_voice_provider_stt_model(s_config.stt_provider, legacy_stt_model_value);
        }
    }
    if (has_legacy_tts_voice_name) {
        config_voice_profile_slots_t tts_profile = {0};

        if (config_get_voice_profile_slots(s_config.tts_provider, &tts_profile) &&
            tts_profile.tts_voice.value != NULL &&
            tts_profile.tts_voice.value[0] == '\0') {
            (void)config_update_voice_provider_tts_voice(s_config.tts_provider, legacy_tts_voice_name);
        }
    }

    config_apply_active_chat_profile();
    config_apply_active_voice_profiles();

    ESP_LOGI(TAG, "Config loaded from NVS");
    return ESP_OK;
}

esp_err_t config_save_to_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    ESP_RETURN_ON_ERROR(err, TAG, "NVS open failed");

    nvs_set_str(nvs_handle, "wifi_ssid", s_config.wifi_ssid);
    nvs_set_str(nvs_handle, "wifi_password", s_config.wifi_password);

    config_apply_active_chat_profile();
    config_apply_active_voice_profiles();

    nvs_set_u8(nvs_handle, "provider", (uint8_t)s_config.provider);

    nvs_set_str(nvs_handle, "oa_key", s_config.openai_api_key);
    nvs_set_str(nvs_handle, "oa_url", s_config.openai_base_url);
    nvs_set_str(nvs_handle, "oa_model", s_config.openai_model_name);

    nvs_set_str(nvs_handle, "zp_key", s_config.zhipu_api_key);
    nvs_set_str(nvs_handle, "zp_url", s_config.zhipu_base_url);
    nvs_set_str(nvs_handle, "zp_model", s_config.zhipu_model_name);

    nvs_set_str(nvs_handle, "ds_key", s_config.deepseek_api_key);
    nvs_set_str(nvs_handle, "ds_url", s_config.deepseek_base_url);
    nvs_set_str(nvs_handle, "ds_model", s_config.deepseek_model_name);

    nvs_erase_key(nvs_handle, "api_key");
    nvs_erase_key(nvs_handle, "base_url");
    nvs_erase_key(nvs_handle, "model_name");

    nvs_erase_key(nvs_handle, "vce_key");
    nvs_erase_key(nvs_handle, "stt_api_key");
    nvs_erase_key(nvs_handle, "stt_base_url");
    nvs_erase_key(nvs_handle, "stt_model_name");
    nvs_erase_key(nvs_handle, "tts_api_key");
    nvs_erase_key(nvs_handle, "tts_base_url");
    nvs_erase_key(nvs_handle, "tts_voice_name");
    nvs_set_str(nvs_handle, "vce_appid", s_config.voice_volcengine_app_id);
    nvs_set_str(nvs_handle, "vce_atok", s_config.voice_volcengine_access_token);
    nvs_set_str(nvs_handle, "vce_skey", s_config.voice_volcengine_secret_key);
    nvs_set_str(nvs_handle, "vce_stt_url", s_config.voice_volcengine_stt_base_url);
    nvs_set_str(nvs_handle, "vce_stt_model", s_config.voice_volcengine_stt_model);
    nvs_set_str(nvs_handle, "vce_tts_url", s_config.voice_volcengine_tts_base_url);
    nvs_set_str(nvs_handle, "vce_tts_voice", s_config.voice_volcengine_tts_voice);

    nvs_set_str(nvs_handle, "aly_key", s_config.voice_aliyun_api_key);
    nvs_set_str(nvs_handle, "aly_stt_url", s_config.voice_aliyun_stt_base_url);
    nvs_set_str(nvs_handle, "aly_stt_model", s_config.voice_aliyun_stt_model);
    nvs_set_str(nvs_handle, "aly_tts_url", s_config.voice_aliyun_tts_base_url);
    nvs_set_str(nvs_handle, "aly_tts_voice", s_config.voice_aliyun_tts_voice);

    nvs_set_str(nvs_handle, "cus_key", s_config.voice_custom_api_key);
    nvs_set_str(nvs_handle, "cus_stt_url", s_config.voice_custom_stt_base_url);
    nvs_set_str(nvs_handle, "cus_stt_model", s_config.voice_custom_stt_model);
    nvs_set_str(nvs_handle, "cus_tts_url", s_config.voice_custom_tts_base_url);
    nvs_set_str(nvs_handle, "cus_tts_voice", s_config.voice_custom_tts_voice);

    nvs_set_u8(nvs_handle, "stt_provider", (uint8_t)s_config.stt_provider);

    nvs_set_u8(nvs_handle, "tts_provider", (uint8_t)s_config.tts_provider);

    nvs_set_str(nvs_handle, "voice_gateway_url", s_config.voice_gateway_url);
    nvs_set_str(nvs_handle, "voice_gateway_token", s_config.voice_gateway_token);
    nvs_set_u8(nvs_handle, "enable_voice_gateway", s_config.enable_voice_gateway ? 1 : 0);

    nvs_set_u8(nvs_handle, "enable_barge_in", s_config.enable_barge_in ? 1 : 0);
    nvs_set_i32(nvs_handle, "vad_silence_ms", s_config.vad_silence_ms);
    nvs_set_i32(nvs_handle, "stt_timeout_ms", s_config.stt_timeout_ms);
    nvs_set_i32(nvs_handle, "tts_timeout_ms", s_config.tts_timeout_ms);
    nvs_set_i32(nvs_handle, "audio_chunk_ms", s_config.audio_chunk_ms);

    nvs_set_str(nvs_handle, "proxy_url", s_config.proxy_url);
    nvs_set_u8(nvs_handle, "enable_proxy", s_config.enable_proxy ? 1 : 0);

    nvs_set_i32(nvs_handle, "temperature_milli", (int32_t)(s_config.temperature * 1000.0f));
    nvs_set_i32(nvs_handle, "max_tokens", s_config.max_tokens);
    nvs_set_i32(nvs_handle, "history_limit", s_config.history_limit);
    nvs_set_i32(nvs_handle, "volume", s_config.volume);
    nvs_set_i32(nvs_handle, "sampling_rate", s_config.sampling_rate);

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config saved to NVS");
    }

    return err;
}

esp_err_t config_factory_reset(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_erase_all(nvs_handle);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    config_reset_to_defaults();

    ESP_LOGI(TAG, "Config factory reset");
    return config_save_to_nvs();
}

app_config_t *config_get(void)
{
    return &s_config;
}

esp_err_t config_set_wifi(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    config_copy_string(s_config.wifi_ssid, sizeof(s_config.wifi_ssid), ssid);
    config_copy_string(s_config.wifi_password, sizeof(s_config.wifi_password), password);
    return config_save_to_nvs();
}

esp_err_t config_set_ai_provider(ai_provider_config_t provider)
{
    if (provider >= AI_PROVIDER_CONFIG_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config.provider = provider;
    config_apply_active_chat_profile();
    return config_save_to_nvs();
}

esp_err_t config_set_stt_provider(voice_provider_config_t provider)
{
    if (provider >= VOICE_PROVIDER_CONFIG_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config.stt_provider = provider;
    config_apply_active_voice_profiles();
    return config_save_to_nvs();
}

esp_err_t config_set_tts_provider(voice_provider_config_t provider)
{
    if (provider >= VOICE_PROVIDER_CONFIG_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config.tts_provider = provider;
    config_apply_active_voice_profiles();
    return config_save_to_nvs();
}

esp_err_t config_set_voice_provider_profile(voice_provider_config_t provider,
                                            const char *api_key,
                                            const char *stt_base_url,
                                            const char *tts_base_url)
{
    if (provider >= VOICE_PROVIDER_CONFIG_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (api_key == NULL && stt_base_url == NULL && tts_base_url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = config_update_voice_provider_profile(provider, api_key, stt_base_url, tts_base_url);
    if (err != ESP_OK) {
        return err;
    }

    if (provider == s_config.stt_provider || provider == s_config.tts_provider) {
        config_apply_active_voice_profiles();
    }

    return config_save_to_nvs();
}

esp_err_t config_set_ai_credentials(const char *api_key, const char *base_url, const char *model_name)
{
    if (api_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return config_set_ai_credentials_for_provider(s_config.provider, api_key, base_url, model_name);
}

esp_err_t config_set_ai_credentials_for_provider(ai_provider_config_t provider,
                                                 const char *api_key,
                                                 const char *base_url,
                                                 const char *model_name)
{
    if (provider >= AI_PROVIDER_CONFIG_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (api_key == NULL && base_url == NULL && model_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = config_update_provider_profile(provider, api_key, base_url, model_name);
    if (err != ESP_OK) {
        return err;
    }

    if (provider == s_config.provider) {
        config_apply_active_chat_profile();
    }

    return config_save_to_nvs();
}

esp_err_t config_set_stt_endpoint(const char *base_url, const char *model_name)
{
    if (base_url == NULL && model_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (base_url != NULL) {
        esp_err_t err = config_update_voice_provider_profile(s_config.stt_provider,
                                                             NULL,
                                                             base_url,
                                                             NULL);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (model_name != NULL) {
        esp_err_t err = config_update_voice_provider_stt_model(s_config.stt_provider, model_name);
        if (err != ESP_OK) {
            return err;
        }
    }

    config_apply_active_voice_profiles();

    return config_save_to_nvs();
}

esp_err_t config_set_tts_endpoint(const char *base_url, const char *voice_name)
{
    if (base_url == NULL && voice_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (base_url != NULL) {
        esp_err_t err = config_update_voice_provider_profile(s_config.tts_provider,
                                                             NULL,
                                                             NULL,
                                                             base_url);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (voice_name != NULL) {
        esp_err_t err = config_update_voice_provider_tts_voice(s_config.tts_provider, voice_name);
        if (err != ESP_OK) {
            return err;
        }
    }

    config_apply_active_voice_profiles();

    return config_save_to_nvs();
}

esp_err_t config_set_voice_gateway(const char *gateway_url, const char *gateway_token, bool enable)
{
    if (enable && (gateway_url == NULL || gateway_url[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    if (gateway_url != NULL) {
        config_copy_string(s_config.voice_gateway_url, sizeof(s_config.voice_gateway_url), gateway_url);
    }
    if (gateway_token != NULL) {
        config_copy_string(s_config.voice_gateway_token, sizeof(s_config.voice_gateway_token), gateway_token);
    }

    s_config.enable_voice_gateway = enable;
    return config_save_to_nvs();
}

esp_err_t config_set_voice_parameters(bool enable_barge_in,
                                      int vad_silence_ms,
                                      int stt_timeout_ms,
                                      int tts_timeout_ms,
                                      int audio_chunk_ms)
{
    if (vad_silence_ms < 100 || stt_timeout_ms < 500 || tts_timeout_ms < 500 ||
        audio_chunk_ms < 10 || audio_chunk_ms > 200) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config.enable_barge_in = enable_barge_in;
    s_config.vad_silence_ms = vad_silence_ms;
    s_config.stt_timeout_ms = stt_timeout_ms;
    s_config.tts_timeout_ms = tts_timeout_ms;
    s_config.audio_chunk_ms = audio_chunk_ms;

    return config_save_to_nvs();
}

esp_err_t config_set_proxy(const char *proxy_url, bool enable)
{
    if (enable && proxy_url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config.enable_proxy = enable;
    if (proxy_url != NULL) {
        config_copy_string(s_config.proxy_url, sizeof(s_config.proxy_url), proxy_url);
    }

    return config_save_to_nvs();
}

esp_err_t config_set_parameters(float temperature, int max_tokens, int history_limit)
{
    if (temperature < 0.0f || temperature > 2.0f || max_tokens <= 0 || history_limit <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_config.temperature = temperature;
    s_config.max_tokens = max_tokens;
    s_config.history_limit = history_limit;
    return config_save_to_nvs();
}
