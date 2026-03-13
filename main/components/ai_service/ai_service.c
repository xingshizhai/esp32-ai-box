#include "ai_service.h"
#include "config.h"
#include "providers/openai.h"
#include "providers/deepseek.h"
#include "providers/zhipu.h"

#include "esp_log.h"
#include "esp_err.h"
#include <stdlib.h>

#define TAG "AI_SERVICE"

ai_service_t* ai_service_create(ai_provider_type_t provider) {
    ai_service_t *service = NULL;
    
    switch (provider) {
        case AI_PROVIDER_OPENAI:
            service = openai_service_create();
            break;
            
        case AI_PROVIDER_ZHIPU:
            service = zhipu_service_create();
            break;
            
        case AI_PROVIDER_DEEPSEEK:
            service = deepseek_service_create();
            break;
            
        case AI_PROVIDER_CUSTOM:
            ESP_LOGE(TAG, "Custom AI provider not yet implemented");
            return NULL;
            
        default:
            ESP_LOGE(TAG, "Invalid AI provider type: %d", provider);
            return NULL;
    }
    
    if (service == NULL) {
        ESP_LOGE(TAG, "Failed to create AI service");
        return NULL;
    }
    
    service->config = NULL;
    ESP_LOGI(TAG, "AI service created");
    
    return service;
}

esp_err_t ai_service_init(ai_service_t *service, const app_config_t *config) {
    if (service == NULL) {
        ESP_LOGE(TAG, "AI service is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (config == NULL) {
        ESP_LOGE(TAG, "Config is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (service->init == NULL) {
        ESP_LOGE(TAG, "Service init function is NULL");
        return ESP_ERR_INVALID_STATE;
    }
    
    service->config = (app_config_t*)config;
    
    ESP_LOGI(TAG, "AI service initializing: provider=%d", config->provider);
    
    return service->init(service, config);
}

void ai_service_destroy(ai_service_t *service) {
    if (service == NULL) {
        ESP_LOGW(TAG, "AI service is NULL");
        return;
    }
    
    if (service->cleanup != NULL) {
        service->cleanup(service);
    }
    
    ESP_LOGI(TAG, "AI service destroyed");
    
    free(service);
}

esp_err_t ai_service_chat(ai_service_t *service, const char *user_message, ai_response_t *response) {
    if (service == NULL || user_message == NULL || response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (service->chat == NULL) {
        ESP_LOGE(TAG, "Chat function not implemented");
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    ESP_LOGI(TAG, "Chat request: %s", user_message);
    
    return service->chat(service, user_message, response);
}

esp_err_t ai_service_chat_with_history(ai_service_t *service, ai_message_t *messages, ai_response_t *response) {
    if (service == NULL || messages == NULL || response == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (service->chat_with_history == NULL) {
        ESP_LOGE(TAG, "Chat with history function not implemented");
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    ESP_LOGI(TAG, "Chat with history");
    
    return service->chat_with_history(service, messages, response);
}

esp_err_t ai_service_stt(ai_service_t *service, const uint8_t *audio, int len, char **text) {
    if (service == NULL || audio == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (service->speech_to_text == NULL) {
        ESP_LOGE(TAG, "STT function not implemented");
        *text = strdup("STT not implemented");
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    ESP_LOGI(TAG, "STT: audio_len=%d", len);
    
    return service->speech_to_text(service, audio, len, text);
}

esp_err_t ai_service_tts(ai_service_t *service, const char *text, uint8_t **audio, int *audio_len) {
    if (service == NULL || text == NULL || audio == NULL || audio_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (service->text_to_speech == NULL) {
        ESP_LOGE(TAG, "TTS function not implemented");
        *audio = NULL;
        *audio_len = 0;
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    ESP_LOGI(TAG, "TTS: text=%s", text);
    
    return service->text_to_speech(service, text, audio, audio_len);
}

ai_message_t* ai_message_create(const char *role, const char *content) {
    ai_message_t *msg = (ai_message_t*)malloc(sizeof(ai_message_t));
    if (msg == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for message");
        return NULL;
    }
    
    msg->role = role ? strdup(role) : strdup("user");
    msg->content = content ? strdup(content) : strdup("");
    msg->next = NULL;
    
    return msg;
}

void ai_message_destroy(ai_message_t *msg) {
    if (msg == NULL) {
        return;
    }
    
    if (msg->role) {
        free((void*)msg->role);
    }
    if (msg->content) {
        free((void*)msg->content);
    }
    free(msg);
}

void ai_message_list_destroy(ai_message_list_t *list) {
    if (list == NULL) {
        return;
    }
    
    ai_message_t *msg = list->head;
    while (msg != NULL) {
        ai_message_t *next = msg->next;
        ai_message_destroy(msg);
        msg = next;
    }
    
    list->head = NULL;
    list->tail = NULL;
    list->count = 0;
}

esp_err_t ai_message_list_append(ai_message_list_t *list, const char *role, const char *content) {
    if (list == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ai_message_t *msg = ai_message_create(role, content);
    if (msg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    
    if (list->head == NULL) {
        list->head = msg;
        list->tail = msg;
    } else {
        list->tail->next = msg;
        list->tail = msg;
    }
    
    list->count++;
    
    return ESP_OK;
}