#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AI_MAX_MODEL_NAME      32
#define AI_MAX_API_KEY         128
#define AI_MAX_BASE_URL        128
#define AI_MAX_RESPONSE_SIZE   4096

typedef enum {
    AI_PROVIDER_OPENAI = 0,
    AI_PROVIDER_ZHIPU,
    AI_PROVIDER_DEEPSEEK,
    AI_PROVIDER_CUSTOM,
    AI_PROVIDER_MAX
} ai_provider_type_t;

typedef enum {
    AI_MESSAGE_ROLE_SYSTEM,
    AI_MESSAGE_ROLE_USER,
    AI_MESSAGE_ROLE_ASSISTANT
} ai_message_role_t;

typedef struct app_config_t app_config_t;
typedef app_config_t ai_config_t;

typedef struct ai_service_t ai_service_t;

typedef struct ai_message {
    const char *role;
    const char *content;
    struct ai_message *next;
} ai_message_t;

typedef struct {
    ai_message_t *head;
    ai_message_t *tail;
    int count;
} ai_message_list_t;

typedef struct {
    char content[AI_MAX_RESPONSE_SIZE];
    bool is_success;
    int tokens_used;
    char error_msg[256];
} ai_response_t;

typedef esp_err_t (*ai_service_init_fn)(ai_service_t *service, const app_config_t *config);
typedef esp_err_t (*ai_service_chat_fn)(ai_service_t *service, const char *user_message, ai_response_t *response);
typedef esp_err_t (*ai_service_chat_history_fn)(ai_service_t *service, ai_message_t *messages, ai_response_t *response);
typedef esp_err_t (*ai_service_stt_fn)(ai_service_t *service, const uint8_t *audio, int len, char **text);
typedef esp_err_t (*ai_service_tts_fn)(ai_service_t *service, const char *text, uint8_t **audio, int *audio_len);
typedef esp_err_t (*ai_service_cleanup_fn)(ai_service_t *service);

struct ai_service_t {
    ai_service_init_fn init;
    ai_service_chat_fn chat;
    ai_service_chat_history_fn chat_with_history;
    ai_service_stt_fn speech_to_text;
    ai_service_tts_fn text_to_speech;
    ai_service_cleanup_fn cleanup;
    
    void *private_data;
    app_config_t *config;
};

ai_service_t* ai_service_create(ai_provider_type_t provider);
esp_err_t ai_service_init(ai_service_t *service, const app_config_t *config);
esp_err_t ai_service_chat(ai_service_t *service, const char *user_message, ai_response_t *response);
esp_err_t ai_service_chat_with_history(ai_service_t *service, ai_message_t *messages, ai_response_t *response);
esp_err_t ai_service_stt(ai_service_t *service, const uint8_t *audio, int len, char **text);
esp_err_t ai_service_tts(ai_service_t *service, const char *text, uint8_t **audio, int *audio_len);
void ai_service_destroy(ai_service_t *service);

ai_message_t* ai_message_create(const char *role, const char *content);
void ai_message_destroy(ai_message_t *msg);
void ai_message_list_destroy(ai_message_list_t *list);
esp_err_t ai_message_list_append(ai_message_list_t *list, const char *role, const char *content);

#ifdef __cplusplus
}
#endif