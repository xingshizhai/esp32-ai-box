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
        ai_message_t *old_msg = conv->messages;
        conv->messages = conv->messages->next;
        old_msg->next = NULL;
        ai_message_destroy(old_msg);
        conv->message_count--;
        ESP_LOGD(TAG, "Removed oldest message to maintain history limit");
    }

    ESP_LOGD(TAG, "Added message: role=%s, count=%d", role, conv->message_count);
    return ESP_OK;
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
