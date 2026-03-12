/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdlib.h>

#define NVS_NAMESPACE "ai_chat"
#define ESP_RETURN_ON_ERROR(x, tag, msg) do { \
    esp_err_t err_ = (x); \
    if (err_ != ESP_OK) { \
        ESP_LOGE(tag, "%s: %s", msg, esp_err_to_name(err_)); \
        return err_; \
    } \
} while(0)

static const char *TAG = "config";
static app_config_t s_config = {
    .wifi_ssid = CONFIG_WIFI_SSID,
    .wifi_password = CONFIG_WIFI_PASSWORD,
    .provider = AI_PROVIDER_CONFIG_DEEPSEEK,
    .api_key = "",
    .base_url = "https://api.deepseek.com/v1/chat/completions",
    .model_name = "deepseek-chat",
    .proxy_url = "",
    .enable_proxy = false,
    .temperature = 0.7f,
    .max_tokens = 2000,
    .history_limit = 10,
    .volume = 80,
    .sampling_rate = 16000
};

esp_err_t config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS init failed");
    
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

    size_t required_size = WIFI_SSID_SIZE;

    nvs_get_str(nvs_handle, "wifi_ssid", s_config.wifi_ssid, &required_size);

    required_size = WIFI_PASSWORD_SIZE;
    nvs_get_str(nvs_handle, "wifi_password", s_config.wifi_password, &required_size);

    nvs_get_u8(nvs_handle, "provider", (uint8_t *)&s_config.provider);

    required_size = API_KEY_SIZE;
    nvs_get_str(nvs_handle, "api_key", s_config.api_key, &required_size);

    required_size = BASE_URL_SIZE;
    nvs_get_str(nvs_handle, "base_url", s_config.base_url, &required_size);

    required_size = MODEL_NAME_SIZE;
    nvs_get_str(nvs_handle, "model_name", s_config.model_name, &required_size);

    required_size = PROXY_URL_SIZE;
    nvs_get_str(nvs_handle, "proxy_url", s_config.proxy_url, &required_size);

    nvs_get_u8(nvs_handle, "enable_proxy", (uint8_t *)&s_config.enable_proxy);
    nvs_get_i32(nvs_handle, "temperature", (int32_t *)&s_config.temperature);
    nvs_get_i32(nvs_handle, "max_tokens", (int32_t *)&s_config.max_tokens);
    nvs_get_i32(nvs_handle, "history_limit", (int32_t *)&s_config.history_limit);
    nvs_get_i32(nvs_handle, "volume", (int32_t *)&s_config.volume);
    nvs_get_i32(nvs_handle, "sampling_rate", (int32_t *)&s_config.sampling_rate);

    nvs_close(nvs_handle);

    if (strlen(s_config.wifi_ssid) == 0 && strlen(CONFIG_WIFI_SSID) > 0) {
        strncpy(s_config.wifi_ssid, CONFIG_WIFI_SSID, WIFI_SSID_SIZE - 1);
        s_config.wifi_ssid[WIFI_SSID_SIZE - 1] = '\0';
    }

    if (strlen(s_config.wifi_password) == 0 && strlen(CONFIG_WIFI_PASSWORD) > 0) {
        strncpy(s_config.wifi_password, CONFIG_WIFI_PASSWORD, WIFI_PASSWORD_SIZE - 1);
        s_config.wifi_password[WIFI_PASSWORD_SIZE - 1] = '\0';
    }

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
    nvs_set_u8(nvs_handle, "provider", s_config.provider);
    nvs_set_str(nvs_handle, "api_key", s_config.api_key);
    nvs_set_str(nvs_handle, "base_url", s_config.base_url);
    nvs_set_str(nvs_handle, "model_name", s_config.model_name);
    nvs_set_str(nvs_handle, "proxy_url", s_config.proxy_url);
    nvs_set_u8(nvs_handle, "enable_proxy", s_config.enable_proxy);
    nvs_set_i32(nvs_handle, "temperature", (int32_t)s_config.temperature);
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
    
    memset(&s_config, 0, sizeof(app_config_t));
    strncpy(s_config.wifi_ssid, CONFIG_WIFI_SSID, WIFI_SSID_SIZE - 1);
    s_config.wifi_ssid[WIFI_SSID_SIZE - 1] = '\0';
    strncpy(s_config.wifi_password, CONFIG_WIFI_PASSWORD, WIFI_PASSWORD_SIZE - 1);
    s_config.wifi_password[WIFI_PASSWORD_SIZE - 1] = '\0';
    s_config.provider = AI_PROVIDER_CONFIG_DEEPSEEK;
    strcpy(s_config.base_url, "https://api.deepseek.com/v1/chat/completions");
    strcpy(s_config.model_name, "deepseek-chat");
    s_config.temperature = 0.7f;
    s_config.max_tokens = 2000;
    s_config.history_limit = 10;
    s_config.volume = 80;
    s_config.sampling_rate = 16000;
    
    ESP_LOGI(TAG, "Config factory reset");
    return ESP_OK;
}

app_config_t* config_get(void)
{
    return &s_config;
}

esp_err_t config_set_wifi(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    strncpy(s_config.wifi_ssid, ssid, WIFI_SSID_SIZE - 1);
    strncpy(s_config.wifi_password, password, WIFI_PASSWORD_SIZE - 1);
    return config_save_to_nvs();
}

esp_err_t config_set_ai_provider(ai_provider_config_t provider)
{
    if (provider >= AI_PROVIDER_CONFIG_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    s_config.provider = provider;
    return config_save_to_nvs();
}

esp_err_t config_set_ai_credentials(const char *api_key, const char *base_url, const char *model_name)
{
    if (api_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    strncpy(s_config.api_key, api_key, API_KEY_SIZE - 1);
    
    if (base_url != NULL) {
        strncpy(s_config.base_url, base_url, BASE_URL_SIZE - 1);
    }
    
    if (model_name != NULL) {
        strncpy(s_config.model_name, model_name, MODEL_NAME_SIZE - 1);
    }
    
    return config_save_to_nvs();
}

esp_err_t config_set_proxy(const char *proxy_url, bool enable)
{
    if (enable && proxy_url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    s_config.enable_proxy = enable;
    if (proxy_url != NULL) {
        strncpy(s_config.proxy_url, proxy_url, PROXY_URL_SIZE - 1);
    }
    
    return config_save_to_nvs();
}

esp_err_t config_set_parameters(float temperature, int max_tokens, int history_limit)
{
    s_config.temperature = temperature;
    s_config.max_tokens = max_tokens;
    s_config.history_limit = history_limit;
    return config_save_to_nvs();
}
