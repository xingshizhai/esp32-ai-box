#pragma once

#include "ai_service.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *api_key;
    char *base_url;
    char *model;
    float temperature;
    int max_tokens;
    esp_http_client_handle_t http_client;
} openai_service_data_t;

ai_service_t* openai_service_create(void);

#ifdef __cplusplus
}
#endif
