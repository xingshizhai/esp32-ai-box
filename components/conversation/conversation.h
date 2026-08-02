#pragma once

#include <stdbool.h>
#include "ai_service.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONVERSATION_MAX_HISTORY 20
#define CONVERSATION_SESSION_ID_LEN 32

typedef enum {
    CONV_STATE_IDLE,
    CONV_STATE_RECORDING,
    CONV_STATE_PROCESSING,
    CONV_STATE_PLAYING,
    CONV_STATE_ERROR
} conversation_state_t;

typedef struct {
    char session_id[CONVERSATION_SESSION_ID_LEN];
    ai_message_t *messages;
    int message_count;
    int max_history;
    conversation_state_t state;
    bool enable_history;
} conversation_manager_t;

esp_err_t conversation_init(conversation_manager_t *conv, int max_history);
esp_err_t conversation_add_message(conversation_manager_t *conv, const char *role, const char *content);
esp_err_t conversation_get_messages(conversation_manager_t *conv, ai_message_t **messages);
esp_err_t conversation_clear(conversation_manager_t *conv);
esp_err_t conversation_set_state(conversation_manager_t *conv, conversation_state_t state);
conversation_state_t conversation_get_state(conversation_manager_t *conv);
void conversation_cleanup(conversation_manager_t *conv);

#ifdef __cplusplus
}
#endif
