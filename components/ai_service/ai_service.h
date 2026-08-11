#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AI_MAX_MODEL_NAME      32
#define AI_MAX_API_KEY         384
#define AI_MAX_BASE_URL        128
#define AI_MAX_RESPONSE_SIZE   4096

#define AI_MAX_TOOL_CALLS      4
#define AI_MAX_TOOL_NAME       32
#define AI_MAX_TOOL_ARGS       256
#define AI_MAX_TOOL_CALL_ID    64

typedef enum {
    AI_PROVIDER_OPENAI = 0,
    AI_PROVIDER_ZHIPU,
    AI_PROVIDER_DEEPSEEK,
    AI_PROVIDER_KIMI,
    AI_PROVIDER_MINIMAX,
    AI_PROVIDER_OPENROUTER,
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

/* One tool call the model asked to run: OpenAI-style {id, function{name, arguments}}. */
typedef struct {
    char id[AI_MAX_TOOL_CALL_ID];
    char name[AI_MAX_TOOL_NAME];
    char arguments[AI_MAX_TOOL_ARGS];   /* raw JSON object, e.g. {"level":80} */
} ai_tool_call_t;

typedef struct ai_message {
    const char *role;
    const char *content;

    /* Set only on role=="tool" messages: the id of the call being answered. */
    const char *tool_call_id;
    /* Set only on role=="assistant" messages that requested tool calls
     * (echoed back verbatim so the provider can match up the follow-up
     * "tool" messages); content is typically empty on these. */
    ai_tool_call_t tool_calls[AI_MAX_TOOL_CALLS];
    int tool_call_count;

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

    /* >0 means the model asked to call tool(s) instead of (or in addition
     * to) replying with content. */
    ai_tool_call_t tool_calls[AI_MAX_TOOL_CALLS];
    int tool_call_count;
} ai_response_t;

typedef esp_err_t (*ai_service_init_fn)(ai_service_t *service, const app_config_t *config);
typedef esp_err_t (*ai_service_chat_fn)(ai_service_t *service, const char *user_message, ai_response_t *response);
typedef esp_err_t (*ai_service_chat_history_fn)(ai_service_t *service, ai_message_t *messages, ai_response_t *response);
typedef esp_err_t (*ai_service_chat_tools_fn)(ai_service_t *service, ai_message_t *messages, const char *tools_json, ai_response_t *response);
typedef esp_err_t (*ai_service_stt_fn)(ai_service_t *service, const uint8_t *audio, int len, char **text);
typedef esp_err_t (*ai_service_tts_fn)(ai_service_t *service, const char *text, uint8_t **audio, int *audio_len);
typedef esp_err_t (*ai_service_cleanup_fn)(ai_service_t *service);

struct ai_service_t {
    ai_service_init_fn init;
    ai_service_chat_fn chat;
    ai_service_chat_history_fn chat_with_history;
    ai_service_chat_tools_fn chat_with_tools;  /* optional; NULL if the provider doesn't support tools */
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
/* Like chat_with_history, but also advertises tools_json (an OpenAI-style
 * "tools" JSON array, or NULL for none). Falls back to chat_with_history
 * (tools ignored) if the provider doesn't set chat_with_tools. */
esp_err_t ai_service_chat_with_tools(ai_service_t *service, ai_message_t *messages, const char *tools_json, ai_response_t *response);
esp_err_t ai_service_stt(ai_service_t *service, const uint8_t *audio, int len, char **text);
esp_err_t ai_service_tts(ai_service_t *service, const char *text, uint8_t **audio, int *audio_len);
void ai_service_destroy(ai_service_t *service);

ai_message_t* ai_message_create(const char *role, const char *content);
ai_message_t* ai_message_create_tool_result(const char *tool_call_id, const char *content);
ai_message_t* ai_message_create_assistant_tool_calls(const ai_tool_call_t *calls, int count);
void ai_message_destroy(ai_message_t *msg);
void ai_message_list_destroy(ai_message_list_t *list);
esp_err_t ai_message_list_append(ai_message_list_t *list, const char *role, const char *content);

#ifdef __cplusplus
}
#endif