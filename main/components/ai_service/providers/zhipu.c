#include "zhipu.h"
#include "config.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "zhipu_service";

typedef struct {
    char *api_key;
    char *base_url;
    char *model;
    float temperature;
    int max_tokens;
} zhipu_service_data_t;

static esp_err_t zhipu_generate_jwt(const char *api_key, char *jwt_token, size_t token_len);

static esp_err_t zhipu_service_init(ai_service_t *service, const ai_config_t *config);
static esp_err_t zhipu_service_chat(ai_service_t *service, const char *user_message, ai_response_t *response);
static esp_err_t zhipu_service_chat_with_history(ai_service_t *service, ai_message_t *messages, ai_response_t *response);
static esp_err_t zhipu_service_cleanup(ai_service_t *service);
static char* build_zhipu_payload(const char *model, const char *user_message, float temperature, int max_tokens);
static char* build_zhipu_history_payload(const char *model, ai_message_t *messages, float temperature, int max_tokens);
static esp_err_t parse_zhipu_response(const char *response_body, ai_response_t *response);

ai_service_t* zhipu_service_create(void)
{
    ai_service_t *service = (ai_service_t *)calloc(1, sizeof(ai_service_t));
    if (service == NULL) {
        return NULL;
    }

    service->init = zhipu_service_init;
    service->chat = zhipu_service_chat;
    service->chat_with_history = zhipu_service_chat_with_history;
    service->speech_to_text = NULL;
    service->text_to_speech = NULL;
    service->cleanup = zhipu_service_cleanup;
    service->private_data = NULL;

    return service;
}

static esp_err_t zhipu_service_init(ai_service_t *service, const ai_config_t *config)
{
    zhipu_service_data_t *data = (zhipu_service_data_t *)calloc(1, sizeof(zhipu_service_data_t));
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    data->api_key = strdup(config->api_key);
    data->base_url = strdup(config->base_url);
    data->model = strdup(config->model_name);
    data->temperature = config->temperature;
    data->max_tokens = config->max_tokens;

    if (data->api_key == NULL || data->base_url == NULL || data->model == NULL) {
        free(data->api_key);
        free(data->base_url);
        free(data->model);
        free(data);
        return ESP_ERR_NO_MEM;
    }

    service->private_data = data;
    ESP_LOGI(TAG, "Zhipu AI service initialized with model: %s", data->model);
    return ESP_OK;
}

static esp_err_t zhipu_service_chat(ai_service_t *service, const char *user_message, ai_response_t *response)
{
    if (service == NULL || service->private_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    zhipu_service_data_t *data = (zhipu_service_data_t *)service->private_data;
    char *payload = build_zhipu_payload(data->model, user_message, data->temperature, data->max_tokens);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char jwt_token[512];
    esp_err_t err = zhipu_generate_jwt(data->api_key, jwt_token, sizeof(jwt_token));
    if (err != ESP_OK) {
        free(payload);
        return err;
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

    char auth_header[768];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", jwt_token);

    esp_http_client_set_header(http_client, "Content-Type", "application/json");
    esp_http_client_set_header(http_client, "Authorization", auth_header);
    esp_http_client_set_post_field(http_client, payload, strlen(payload));

    err = esp_http_client_perform(http_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        free(payload);
        esp_http_client_cleanup(http_client);
        return err;
    }

    int status = esp_http_client_get_status_code(http_client);
    int content_length = esp_http_client_get_content_length(http_client);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP error: %d", status);
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

    err = parse_zhipu_response(response_body, response);
    free(response_body);

    return err;
}

static esp_err_t zhipu_service_chat_with_history(ai_service_t *service, ai_message_t *messages, ai_response_t *response)
{
    if (service == NULL || service->private_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    zhipu_service_data_t *data = (zhipu_service_data_t *)service->private_data;
    char *payload = build_zhipu_history_payload(data->model, messages, data->temperature, data->max_tokens);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char jwt_token[512];
    esp_err_t err = zhipu_generate_jwt(data->api_key, jwt_token, sizeof(jwt_token));
    if (err != ESP_OK) {
        free(payload);
        return err;
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

    char auth_header[768];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", jwt_token);

    esp_http_client_set_header(http_client, "Content-Type", "application/json");
    esp_http_client_set_header(http_client, "Authorization", auth_header);
    esp_http_client_set_post_field(http_client, payload, strlen(payload));

    err = esp_http_client_perform(http_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        free(payload);
        esp_http_client_cleanup(http_client);
        return err;
    }

    int status = esp_http_client_get_status_code(http_client);
    int content_length = esp_http_client_get_content_length(http_client);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP error: %d", status);
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

    err = parse_zhipu_response(response_body, response);
    free(response_body);

    return err;
}

static esp_err_t zhipu_service_cleanup(ai_service_t *service)
{
    if (service == NULL || service->private_data == NULL) {
        return ESP_OK;
    }

    zhipu_service_data_t *data = (zhipu_service_data_t *)service->private_data;

    if (data->api_key != NULL) {
        free(data->api_key);
    }
    if (data->base_url != NULL) {
        free(data->base_url);
    }
    if (data->model != NULL) {
        free(data->model);
    }

    free(data);
    service->private_data = NULL;

    return ESP_OK;
}

static char* build_zhipu_payload(const char *model, const char *user_message, float temperature, int max_tokens)
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

static char* build_zhipu_history_payload(const char *model, ai_message_t *messages, float temperature, int max_tokens)
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

static esp_err_t parse_zhipu_response(const char *response_body, ai_response_t *response)
{
    cJSON *root = cJSON_Parse(response_body);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_FAIL;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (choices == NULL || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        ESP_LOGE(TAG, "Invalid response format");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(first_choice, "message");
    if (message == NULL) {
        ESP_LOGE(TAG, "No message in response");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content == NULL || !cJSON_IsString(content)) {
        ESP_LOGE(TAG, "No content in message");
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

static esp_err_t zhipu_generate_jwt(const char *api_key, char *jwt_token, size_t token_len)
{
    const char *delimiter = ".";
    char *secret = strstr(api_key, delimiter);
    if (secret == NULL) {
        ESP_LOGE(TAG, "Invalid API key format");
        return ESP_ERR_INVALID_ARG;
    }

    int id_len = secret - api_key;
    char *id = (char *)malloc(id_len + 1);
    memcpy(id, api_key, id_len);
    id[id_len] = '\0';
    secret++;

    snprintf(jwt_token, token_len, "Bearer %s.%s", id, secret);

    free(id);
    return ESP_OK;
}
