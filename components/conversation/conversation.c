#include "conversation.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "conversation";

esp_err_t conversation_init(conversation_manager_t *conv, int max_history)
{
    if (conv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(conv, 0, sizeof(conversation_manager_t));

    conv->message_count = 0;
    conv->max_history = (max_history > 0 && max_history <= CONVERSATION_MAX_HISTORY) ? max_history : CONVERSATION_MAX_HISTORY;
    conv->messages = NULL;
    conv->state = CONV_STATE_IDLE;
    conv->enable_history = true;

    snprintf(conv->session_id, sizeof(conv->session_id), "session_%08x", (unsigned int)xTaskGetTickCount());

    ESP_LOGI(TAG, "Conversation manager initialized with session: %s", conv->session_id);
    return ESP_OK;
}

static esp_err_t conversation_append_and_trim(conversation_manager_t *conv, ai_message_t *new_msg)
{
    if (conv->messages == NULL) {
        conv->messages = new_msg;
        conv->message_count = 1;
    } else {
        ai_message_t *current = conv->messages;
        int count = 1;
        while (current->next != NULL) {
            current = current->next;
            count++;
        }
        current->next = new_msg;
        conv->message_count = count + 1;
    }

    if (conv->message_count > conv->max_history) {
        /* Keep a leading "system" message pinned: evict the oldest message
         * after it instead, so the voice-style prompt survives long
         * sessions instead of aging out of the FIFO like a normal turn. */
        ai_message_t *victim_prev = NULL;
        ai_message_t *victim = conv->messages;
        if (victim != NULL && victim->role != NULL && strcmp(victim->role, "system") == 0) {
            victim_prev = victim;
            victim = victim->next;
        }

        if (victim != NULL) {
            if (victim_prev != NULL) {
                victim_prev->next = victim->next;
            } else {
                conv->messages = victim->next;
            }
            victim->next = NULL;
            ai_message_destroy(victim);
            conv->message_count--;
            ESP_LOGD(TAG, "Removed oldest non-system message to maintain history limit");
        }
    }

    ESP_LOGD(TAG, "Added message: role=%s, count=%d", new_msg->role, conv->message_count);
    return ESP_OK;
}

esp_err_t conversation_add_message(conversation_manager_t *conv, const char *role, const char *content)
{
    if (conv == NULL || role == NULL || content == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!conv->enable_history) {
        return ESP_OK;
    }

    ai_message_t *new_msg = ai_message_create(role, content);
    if (new_msg == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return conversation_append_and_trim(conv, new_msg);
}

esp_err_t conversation_add_assistant_tool_calls(conversation_manager_t *conv, const ai_tool_call_t *calls, int count)
{
    if (conv == NULL || calls == NULL || count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!conv->enable_history) {
        return ESP_OK;
    }

    ai_message_t *new_msg = ai_message_create_assistant_tool_calls(calls, count);
    if (new_msg == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return conversation_append_and_trim(conv, new_msg);
}

esp_err_t conversation_add_tool_result(conversation_manager_t *conv, const char *tool_call_id, const char *content)
{
    if (conv == NULL || tool_call_id == NULL || content == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!conv->enable_history) {
        return ESP_OK;
    }

    ai_message_t *new_msg = ai_message_create_tool_result(tool_call_id, content);
    if (new_msg == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return conversation_append_and_trim(conv, new_msg);
}

esp_err_t conversation_get_messages(conversation_manager_t *conv, ai_message_t **messages)
{
    if (conv == NULL || messages == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *messages = conv->messages;
    return ESP_OK;
}

esp_err_t conversation_clear(conversation_manager_t *conv)
{
    if (conv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (conv->messages != NULL) {
        ai_message_t *current = conv->messages;
        while (current != NULL) {
            ai_message_t *next = current->next;
            ai_message_destroy(current);
            current = next;
        }
        conv->messages = NULL;
        conv->message_count = 0;
    }

    ESP_LOGI(TAG, "Conversation cleared for session: %s", conv->session_id);
    return ESP_OK;
}

esp_err_t conversation_set_state(conversation_manager_t *conv, conversation_state_t state)
{
    if (conv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    conv->state = state;
    return ESP_OK;
}

conversation_state_t conversation_get_state(conversation_manager_t *conv)
{
    if (conv == NULL) {
        return CONV_STATE_ERROR;
    }

    return conv->state;
}

void conversation_cleanup(conversation_manager_t *conv)
{
    if (conv == NULL) {
        return;
    }

    conversation_clear(conv);
    ESP_LOGI(TAG, "Conversation manager cleaned up");
}
