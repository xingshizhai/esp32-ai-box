/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

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
#define PROXY_URL_SIZE        128

typedef enum {
    AI_PROVIDER_CONFIG_OPENAI = 0,
    AI_PROVIDER_CONFIG_ZHIPU,
    AI_PROVIDER_CONFIG_DEEPSEEK,
    AI_PROVIDER_CONFIG_MAX
} ai_provider_config_t;

typedef struct app_config_t {
    char wifi_ssid[WIFI_SSID_SIZE];
    char wifi_password[WIFI_PASSWORD_SIZE];
    
    ai_provider_config_t provider;
    char api_key[API_KEY_SIZE];
    char base_url[BASE_URL_SIZE];
    char model_name[MODEL_NAME_SIZE];
    
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
esp_err_t config_set_proxy(const char *proxy_url, bool enable);
esp_err_t config_set_parameters(float temperature, int max_tokens, int history_limit);

#ifdef __cplusplus
}
#endif
