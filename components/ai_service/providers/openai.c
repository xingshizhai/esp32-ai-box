#include "openai.h"
#include "config.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "openai_service";

#define OPENAI_HTTP_ERROR_BODY_MAX 384

static esp_err_t openai_service_init(ai_service_t *service, const ai_config_t *config);
static esp_err_t openai_service_chat(ai_service_t *service, const char *user_message, ai_response_t *response);
static esp_err_t openai_service_chat_with_history(ai_service_t *service, ai_message_t *messages, ai_response_t *response);
static esp_err_t openai_service_cleanup(ai_service_t *service);
static char* build_chat_payload(const char *model, const char *user_message, float temperature, int max_tokens);
static char* build_chat_history_payload(const char *model, ai_message_t *messages, float temperature, int max_tokens);
static esp_err_t parse_chat_response(const char *response_body, ai_response_t *response);

static void openai_response_reset(ai_response_t *response)
{
    if (response == NULL) {
        return;
    }

    response->content[0] = '\0';
    response->is_success = false;
    response->tokens_used = 0;
    response->error_msg[0] = '\0';
}

ai_service_t* openai_service_create(void)
{
    ai_service_t *service = (ai_service_t *)calloc(1, sizeof(ai_service_t));
    if (service == NULL) {
        return NULL;
    }

    service->init = openai_service_init;
    service->chat = openai_service_chat;
    service->chat_with_history = openai_service_chat_with_history;
    service->speech_to_text = NULL;
    service->text_to_speech = NULL;
    service->cleanup = openai_service_cleanup;
    service->private_data = NULL;

    return service;
}

static esp_err_t openai_service_init(ai_service_t *service, const ai_config_t *config)
{
    openai_service_data_t *data = (openai_service_data_t *)calloc(1, sizeof(openai_service_data_t));
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    data->api_key = strdup(config->api_key);
    data->base_url = strdup(config->base_url);
    data->model = strdup(config->model_name);
    data->use_openrouter_headers = (config->provider == AI_PROVIDER_CONFIG_OPENROUTER);
    data->openrouter_http_referer = strdup(config->openrouter_http_referer);
    data->openrouter_x_title = strdup(config->openrouter_x_title);
    data->temperature = config->temperature;
    data->max_tokens = config->max_tokens;

    if (data->api_key == NULL || data->base_url == NULL || data->model == NULL ||
        data->openrouter_http_referer == NULL || data->openrouter_x_title == NULL) {
        free(data->api_key);
        free(data->base_url);
        free(data->model);
        free(data->openrouter_http_referer);
        free(data->openrouter_x_title);
        free(data);
        return ESP_ERR_NO_MEM;
    }

    service->private_data = data;
    ESP_LOGI(TAG, "OpenAI service initialized with model: %s", data->model);
    return ESP_OK;
}

static esp_err_t openai_service_chat(ai_service_t *service, const char *user_message, ai_response_t *response)
{
    if (service == NULL || service->private_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    openai_response_reset(response);

    openai_service_data_t *data = (openai_service_data_t *)service->private_data;
    char *payload = build_chat_payload(data->model, user_message, data->temperature, data->max_tokens);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t http_config = {
        .url = data->base_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t http_client = esp_http_client_init(&http_config);
    if (http_client == NULL) {
        free(payload);
        return ESP_FAIL;
    }

    char auth_header[API_KEY_SIZE + 16];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", data->api_key);

    esp_http_client_set_header(http_client, "Content-Type", "application/json");
    esp_http_client_set_header(http_client, "Authorization", auth_header);
    if (data->use_openrouter_headers) {
        if (data->openrouter_http_referer[0] != '\0') {
            esp_http_client_set_header(http_client, "HTTP-Referer", data->openrouter_http_referer);
        }
        if (data->openrouter_x_title[0] != '\0') {
            esp_http_client_set_header(http_client, "X-Title", data->openrouter_x_title);
        }
    }
    esp_http_client_set_post_field(http_client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(http_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        free(payload);
        esp_http_client_cleanup(http_client);
        return err;
    }

    int status = esp_http_client_get_status_code(http_client);
    int content_length = esp_http_client_get_content_length(http_client);

    if (status != 200) {
        char error_body[OPENAI_HTTP_ERROR_BODY_MAX] = {0};
        int err_len = esp_http_client_read(http_client, error_body, sizeof(error_body) - 1);
        if (err_len < 0) {
            err_len = 0;
        }
        error_body[err_len] = '\0';
        ESP_LOGE(TAG, "HTTP error: %d body=%s", status, error_body);
        snprintf(response->error_msg,
                 sizeof(response->error_msg),
                 "HTTP %d: %.220s",
                 status,
                 (error_body[0] != '\0') ? error_body : "empty response");
        free(payload);
        esp_http_client_cleanup(http_client);
        return ESP_FAIL;
    }

    char *response_body = (char *)malloc(content_length + 1);
    if (response_body == NULL) {
        free(payload);
        esp_http_client_cleanup(http_client);
        return ESP_ERR_NO_MEM;
    }

    int read_len = esp_http_client_read(http_client, response_body, content_length);
    response_body[read_len] = '\0';

    free(payload);
    esp_http_client_cleanup(http_client);

    err = parse_chat_response(response_body, response);
    free(response_body);

    return err;
}

static esp_err_t openai_service_chat_with_history(ai_service_t *service, ai_message_t *messages, ai_response_t *response)
{
    if (service == NULL || service->private_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    openai_response_reset(response);

    openai_service_data_t *data = (openai_service_data_t *)service->private_data;
    char *payload = build_chat_history_payload(data->model, messages, data->temperature, data->max_tokens);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t http_config = {
        .url = data->base_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t http_client = esp_http_client_init(&http_config);
    if (http_client == NULL) {
        free(payload);
        return ESP_FAIL;
    }

    char auth_header[API_KEY_SIZE + 16];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", data->api_key);

    esp_http_client_set_header(http_client, "Content-Type", "application/json");
    esp_http_client_set_header(http_client, "Authorization", auth_header);
    if (data->use_openrouter_headers) {
        if (data->openrouter_http_referer[0] != '\0') {
            esp_http_client_set_header(http_client, "HTTP-Referer", data->openrouter_http_referer);
        }
        if (data->openrouter_x_title[0] != '\0') {
            esp_http_client_set_header(http_client, "X-Title", data->openrouter_x_title);
        }
    }
    esp_http_client_set_post_field(http_client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(http_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        free(payload);
        esp_http_client_cleanup(http_client);
        return err;
    }

    int status = esp_http_client_get_status_code(http_client);
    int content_length = esp_http_client_get_content_length(http_client);

    if (status != 200) {
        char error_body[OPENAI_HTTP_ERROR_BODY_MAX] = {0};
        int err_len = esp_http_client_read(http_client, error_body, sizeof(error_body) - 1);
        if (err_len < 0) {
            err_len = 0;
        }
        error_body[err_len] = '\0';
        ESP_LOGE(TAG, "HTTP error: %d body=%s", status, error_body);
        snprintf(response->error_msg,
                 sizeof(response->error_msg),
                 "HTTP %d: %.220s",
                 status,
                 (error_body[0] != '\0') ? error_body : "empty response");
        free(payload);
        esp_http_client_cleanup(http_client);
        return ESP_FAIL;
    }

    char *response_body = (char *)malloc(content_length + 1);
    if (response_body == NULL) {
        free(payload);
        esp_http_client_cleanup(http_client);
        return ESP_ERR_NO_MEM;
    }

    int read_len = esp_http_client_read(http_client, response_body, content_length);
    response_body[read_len] = '\0';

    free(payload);
    esp_http_client_cleanup(http_client);

    err = parse_chat_response(response_body, response);
    free(response_body);

    return err;
}

static esp_err_t openai_service_cleanup(ai_service_t *service)
{
    if (service == NULL || service->private_data == NULL) {
        return ESP_OK;
    }

    openai_service_data_t *data = (openai_service_data_t *)service->private_data;

    if (data->api_key != NULL) {
        free(data->api_key);
    }
    if (data->base_url != NULL) {
        free(data->base_url);
    }
    if (data->model != NULL) {
        free(data->model);
    }
    if (data->openrouter_http_referer != NULL) {
        free(data->openrouter_http_referer);
    }
    if (data->openrouter_x_title != NULL) {
        free(data->openrouter_x_title);
    }

    free(data);
    service->private_data = NULL;

    return ESP_OK;
}

static char* build_chat_payload(const char *model, const char *user_message, float temperature, int max_tokens)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();

    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", user_message);
    cJSON_AddItemToArray(messages, msg);

    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddItemToObject(root, "messages", messages);
    cJSON_AddNumberToObject(root, "temperature", temperature);
    cJSON_AddNumberToObject(root, "max_tokens", max_tokens);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return payload;
}

static char* build_chat_history_payload(const char *model, ai_message_t *messages, float temperature, int max_tokens)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *msg_array = cJSON_CreateArray();

    ai_message_t *current = messages;
    while (current != NULL) {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", current->role);
        cJSON_AddStringToObject(msg, "content", current->content);
        cJSON_AddItemToArray(msg_array, msg);
        current = current->next;
    }

    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddItemToObject(root, "messages", msg_array);
    cJSON_AddNumberToObject(root, "temperature", temperature);
    cJSON_AddNumberToObject(root, "max_tokens", max_tokens);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return payload;
}

static esp_err_t parse_chat_response(const char *response_body, ai_response_t *response)
{
    cJSON *root = cJSON_Parse(response_body);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        snprintf(response->error_msg, sizeof(response->error_msg), "Invalid JSON response");
        return ESP_FAIL;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (choices == NULL || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        ESP_LOGE(TAG, "Invalid response format");
        snprintf(response->error_msg, sizeof(response->error_msg), "Invalid response format: choices");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(first_choice, "message");
    if (message == NULL) {
        ESP_LOGE(TAG, "No message in response");
        snprintf(response->error_msg, sizeof(response->error_msg), "Invalid response format: message");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content == NULL || !cJSON_IsString(content)) {
        ESP_LOGE(TAG, "No content in message");
        snprintf(response->error_msg, sizeof(response->error_msg), "Invalid response format: content");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strncpy(response->content, content->valuestring, AI_MAX_RESPONSE_SIZE - 1);
    response->content[AI_MAX_RESPONSE_SIZE - 1] = '\0';
    response->is_success = true;

    cJSON *usage = cJSON_GetObjectItem(root, "usage");
    if (usage != NULL) {
        cJSON *total_tokens = cJSON_GetObjectItem(usage, "total_tokens");
        if (total_tokens != NULL && cJSON_IsNumber(total_tokens)) {
            response->tokens_used = total_tokens->valueint;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}
