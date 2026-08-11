#include "deepseek.h"
#include "config.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "deepseek_service";

#define DEEPSEEK_HTTP_RX_INIT_CAP  4096
#define DEEPSEEK_HTTP_RX_MAX_CAP   (256 * 1024)

typedef struct {
    char *api_key;
    char *base_url;
    char *model;
    float temperature;
    int max_tokens;
} deepseek_service_data_t;

typedef struct {
    char *data;
    int len;
    int cap;
} deepseek_http_buf_t;

static esp_err_t deepseek_service_init(ai_service_t *service, const ai_config_t *config);
static esp_err_t deepseek_service_chat(ai_service_t *service, const char *user_message, ai_response_t *response);
static esp_err_t deepseek_service_chat_with_history(ai_service_t *service, ai_message_t *messages, ai_response_t *response);
static esp_err_t deepseek_service_chat_with_tools(ai_service_t *service, ai_message_t *messages, const char *tools_json, ai_response_t *response);
static esp_err_t deepseek_service_cleanup(ai_service_t *service);
static char *build_deepseek_payload(const char *model, const char *user_message, float temperature, int max_tokens);
static char *build_deepseek_history_payload(const char *model, ai_message_t *messages, float temperature, int max_tokens);
static char *build_deepseek_tools_payload(const char *model, ai_message_t *messages, float temperature, int max_tokens, const char *tools_json);
static void add_deepseek_message_array(cJSON *msg_array, ai_message_t *messages);
static esp_err_t parse_deepseek_response(const char *response_body, ai_response_t *response);
static esp_err_t deepseek_http_post_json(const deepseek_service_data_t *data,
                                         const char *payload,
                                         char **out_body,
                                         int *out_status);

ai_service_t *deepseek_service_create(void)
{
    ai_service_t *service = (ai_service_t *)calloc(1, sizeof(ai_service_t));
    if (service == NULL) {
        return NULL;
    }

    service->init = deepseek_service_init;
    service->chat = deepseek_service_chat;
    service->chat_with_history = deepseek_service_chat_with_history;
    service->chat_with_tools = deepseek_service_chat_with_tools;
    service->speech_to_text = NULL;
    service->text_to_speech = NULL;
    service->cleanup = deepseek_service_cleanup;
    service->private_data = NULL;

    return service;
}

static esp_err_t deepseek_service_init(ai_service_t *service, const ai_config_t *config)
{
    deepseek_service_data_t *data = (deepseek_service_data_t *)calloc(1, sizeof(deepseek_service_data_t));
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
    ESP_LOGI(TAG, "DeepSeek service initialized with model: %s", data->model);
    return ESP_OK;
}

static esp_err_t deepseek_http_buf_append(deepseek_http_buf_t *buf, const char *data, int len)
{
    if (buf == NULL || data == NULL || len <= 0) {
        return ESP_OK;
    }

    if (buf->data == NULL) {
        buf->cap = DEEPSEEK_HTTP_RX_INIT_CAP;
        if (buf->cap < len + 1) {
            buf->cap = len + 1;
        }
        buf->data = (char *)malloc((size_t)buf->cap);
        if (buf->data == NULL) {
            return ESP_ERR_NO_MEM;
        }
        buf->len = 0;
    }

    if (buf->len + len + 1 > buf->cap) {
        int next_cap = buf->cap;
        while (next_cap < buf->len + len + 1) {
            next_cap *= 2;
            if (next_cap > DEEPSEEK_HTTP_RX_MAX_CAP) {
                return ESP_ERR_NO_MEM;
            }
        }
        char *resized = (char *)realloc(buf->data, (size_t)next_cap);
        if (resized == NULL) {
            return ESP_ERR_NO_MEM;
        }
        buf->data = resized;
        buf->cap = next_cap;
    }

    memcpy(buf->data + buf->len, data, (size_t)len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return ESP_OK;
}

static esp_err_t deepseek_http_event_handler(esp_http_client_event_t *evt)
{
    deepseek_http_buf_t *buf = (deepseek_http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        return deepseek_http_buf_append(buf, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

static esp_err_t deepseek_http_post_json(const deepseek_service_data_t *data,
                                         const char *payload,
                                         char **out_body,
                                         int *out_status)
{
    if (data == NULL || payload == NULL || out_body == NULL || out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_body = NULL;
    *out_status = 0;

    deepseek_http_buf_t buf = {0};
    esp_http_client_config_t http_config = {
        .url = data->base_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = deepseek_http_event_handler,
        .user_data = &buf,
    };

    esp_http_client_handle_t http_client = esp_http_client_init(&http_config);
    if (http_client == NULL) {
        return ESP_FAIL;
    }

    char auth_header[API_KEY_SIZE + 16];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", data->api_key);
    esp_http_client_set_header(http_client, "Content-Type", "application/json");
    esp_http_client_set_header(http_client, "Authorization", auth_header);
    esp_http_client_set_post_field(http_client, payload, (int)strlen(payload));

    esp_err_t err = esp_http_client_perform(http_client);
    *out_status = esp_http_client_get_status_code(http_client);
    esp_http_client_cleanup(http_client);

    if (err != ESP_OK) {
        free(buf.data);
        return err;
    }

    if (buf.data == NULL) {
        buf.data = strdup("");
        if (buf.data == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    *out_body = buf.data;
    return ESP_OK;
}

static esp_err_t deepseek_service_chat(ai_service_t *service, const char *user_message, ai_response_t *response)
{
    if (service == NULL || service->private_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    deepseek_service_data_t *data = (deepseek_service_data_t *)service->private_data;
    char *payload = build_deepseek_payload(data->model, user_message, data->temperature, data->max_tokens);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char *response_body = NULL;
    int status = 0;
    esp_err_t err = deepseek_http_post_json(data, payload, &response_body, &status);
    free(payload);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        free(response_body);
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP error: %d body=%.200s", status, response_body ? response_body : "");
        if (response != NULL) {
            snprintf(response->error_msg,
                     sizeof(response->error_msg),
                     "HTTP %d: %.220s",
                     status,
                     (response_body && response_body[0]) ? response_body : "empty");
        }
        free(response_body);
        return ESP_FAIL;
    }

    err = parse_deepseek_response(response_body, response);
    free(response_body);
    return err;
}

static esp_err_t deepseek_service_chat_with_history(ai_service_t *service, ai_message_t *messages, ai_response_t *response)
{
    if (service == NULL || service->private_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    deepseek_service_data_t *data = (deepseek_service_data_t *)service->private_data;
    char *payload = build_deepseek_history_payload(data->model, messages, data->temperature, data->max_tokens);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "DeepSeek history request bytes=%u model=%s max_tokens=%d",
             (unsigned)strlen(payload), data->model, data->max_tokens);

    char *response_body = NULL;
    int status = 0;
    esp_err_t err = deepseek_http_post_json(data, payload, &response_body, &status);
    free(payload);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        free(response_body);
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP error: %d body=%.200s", status, response_body ? response_body : "");
        if (response != NULL) {
            snprintf(response->error_msg,
                     sizeof(response->error_msg),
                     "HTTP %d: %.220s",
                     status,
                     (response_body && response_body[0]) ? response_body : "empty");
        }
        free(response_body);
        return ESP_FAIL;
    }

    err = parse_deepseek_response(response_body, response);
    free(response_body);
    return err;
}

static esp_err_t deepseek_service_chat_with_tools(ai_service_t *service, ai_message_t *messages, const char *tools_json, ai_response_t *response)
{
    if (service == NULL || service->private_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    deepseek_service_data_t *data = (deepseek_service_data_t *)service->private_data;
    char *payload = build_deepseek_tools_payload(data->model, messages, data->temperature, data->max_tokens, tools_json);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "DeepSeek tools request bytes=%u model=%s max_tokens=%d",
             (unsigned)strlen(payload), data->model, data->max_tokens);

    char *response_body = NULL;
    int status = 0;
    esp_err_t err = deepseek_http_post_json(data, payload, &response_body, &status);
    free(payload);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        free(response_body);
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP error: %d body=%.200s", status, response_body ? response_body : "");
        if (response != NULL) {
            snprintf(response->error_msg,
                     sizeof(response->error_msg),
                     "HTTP %d: %.220s",
                     status,
                     (response_body && response_body[0]) ? response_body : "empty");
        }
        free(response_body);
        return ESP_FAIL;
    }

    err = parse_deepseek_response(response_body, response);
    free(response_body);
    return err;
}

static esp_err_t deepseek_service_cleanup(ai_service_t *service)
{
    if (service == NULL || service->private_data == NULL) {
        return ESP_OK;
    }

    deepseek_service_data_t *data = (deepseek_service_data_t *)service->private_data;

    free(data->api_key);
    free(data->base_url);
    free(data->model);
    free(data);
    service->private_data = NULL;

    return ESP_OK;
}

static char *build_deepseek_payload(const char *model, const char *user_message, float temperature, int max_tokens)
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

/* Shared by build_deepseek_history_payload / build_deepseek_tools_payload.
 * Handles the three message shapes the DeepSeek (OpenAI-compatible) API
 * expects:
 *   - plain (role/content)
 *   - assistant tool-call request (tool_call_count > 0): "tool_calls" array,
 *     no "content"
 *   - tool result (tool_call_id != NULL): adds "tool_call_id"
 */
static void add_deepseek_message_array(cJSON *msg_array, ai_message_t *messages)
{
    for (ai_message_t *current = messages; current != NULL; current = current->next) {
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", current->role);

        if (current->tool_call_count > 0) {
            cJSON *tool_calls = cJSON_CreateArray();
            for (int i = 0; i < current->tool_call_count; i++) {
                const ai_tool_call_t *call = &current->tool_calls[i];
                cJSON *tc = cJSON_CreateObject();
                cJSON_AddStringToObject(tc, "id", call->id);
                cJSON_AddStringToObject(tc, "type", "function");
                cJSON *fn = cJSON_CreateObject();
                cJSON_AddStringToObject(fn, "name", call->name);
                cJSON_AddStringToObject(fn, "arguments", call->arguments);
                cJSON_AddItemToObject(tc, "function", fn);
                cJSON_AddItemToArray(tool_calls, tc);
            }
            cJSON_AddItemToObject(msg, "tool_calls", tool_calls);
        } else {
            cJSON_AddStringToObject(msg, "content", current->content ? current->content : "");
        }

        if (current->tool_call_id != NULL) {
            cJSON_AddStringToObject(msg, "tool_call_id", current->tool_call_id);
        }

        cJSON_AddItemToArray(msg_array, msg);
    }
}

static char *build_deepseek_history_payload(const char *model, ai_message_t *messages, float temperature, int max_tokens)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *msg_array = cJSON_CreateArray();

    add_deepseek_message_array(msg_array, messages);

    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddItemToObject(root, "messages", msg_array);
    cJSON_AddNumberToObject(root, "temperature", temperature);
    cJSON_AddNumberToObject(root, "max_tokens", max_tokens);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static char *build_deepseek_tools_payload(const char *model, ai_message_t *messages, float temperature, int max_tokens, const char *tools_json)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *msg_array = cJSON_CreateArray();

    add_deepseek_message_array(msg_array, messages);

    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddItemToObject(root, "messages", msg_array);
    cJSON_AddNumberToObject(root, "temperature", temperature);
    cJSON_AddNumberToObject(root, "max_tokens", max_tokens);

    if (tools_json != NULL) {
        cJSON *tools = cJSON_Parse(tools_json);
        if (tools != NULL) {
            cJSON_AddItemToObject(root, "tools", tools);
            cJSON_AddStringToObject(root, "tool_choice", "auto");
        } else {
            ESP_LOGW(TAG, "tools_json failed to parse, sending request without tools");
        }
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static esp_err_t parse_deepseek_response(const char *response_body, ai_response_t *response)
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

    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (tool_calls != NULL && cJSON_IsArray(tool_calls)) {
        int n = cJSON_GetArraySize(tool_calls);
        if (n > AI_MAX_TOOL_CALLS) {
            ESP_LOGW(TAG, "Model requested %d tool calls, truncating to %d", n, AI_MAX_TOOL_CALLS);
            n = AI_MAX_TOOL_CALLS;
        }
        for (int i = 0; i < n; i++) {
            cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
            cJSON *id = cJSON_GetObjectItem(tc, "id");
            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            cJSON *name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
            cJSON *args = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;

            ai_tool_call_t *call = &response->tool_calls[response->tool_call_count];
            memset(call, 0, sizeof(*call));
            if (cJSON_IsString(id)) {
                strncpy(call->id, id->valuestring, AI_MAX_TOOL_CALL_ID - 1);
            }
            if (cJSON_IsString(name)) {
                strncpy(call->name, name->valuestring, AI_MAX_TOOL_NAME - 1);
            }
            if (cJSON_IsString(args)) {
                if (strlen(args->valuestring) >= AI_MAX_TOOL_ARGS) {
                    ESP_LOGW(TAG, "Tool call arguments for %s truncated (%d bytes)",
                             call->name, (int)strlen(args->valuestring));
                }
                strncpy(call->arguments, args->valuestring, AI_MAX_TOOL_ARGS - 1);
            } else {
                strcpy(call->arguments, "{}");
            }
            response->tool_call_count++;
        }
    }

    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content != NULL && cJSON_IsString(content)) {
        strncpy(response->content, content->valuestring, AI_MAX_RESPONSE_SIZE - 1);
        response->content[AI_MAX_RESPONSE_SIZE - 1] = '\0';
    } else if (response->tool_call_count == 0) {
        ESP_LOGE(TAG, "No content and no tool_calls in message");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
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
