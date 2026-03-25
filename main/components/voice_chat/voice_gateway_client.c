#include "voice_gateway_client.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "voice_gateway";

#define VOICE_STT_START_PATH      "/v1/stt/start"
#define VOICE_STT_CHUNK_PATH      "/v1/stt/chunk"
#define VOICE_STT_STOP_PATH       "/v1/stt/stop"
#define VOICE_TTS_PATH            "/v1/tts"

#define VOICE_HTTP_TIMEOUT_MS     15000
#define VOICE_HTTP_RX_INIT_CAP    2048
#define VOICE_HTTP_RX_MAX_CAP     (512 * 1024)

struct voice_gateway_client {
    char *base_url;
    char *access_token;
    int timeout_ms;

    char *stt_provider;
    char *stt_model_name;
    char *stt_api_key;
    char *stt_app_id;
    char *stt_access_token;
    char *stt_secret_key;
    char *stt_base_url;
    char *stt_language;
    bool stt_enable_itn;
    bool stt_enable_punc;
    bool stt_enable_nonstream;
    char *stt_result_type;
    int stt_end_window_size_ms;

    char *tts_provider;
    char *tts_model_name;
    char *tts_api_key;
    char *tts_app_id;
    char *tts_access_token;
    char *tts_secret_key;
    char *tts_base_url;
    char *tts_transport;
};

static char *voice_strdup_or_empty(const char *value)
{
    if (value == NULL) {
        return strdup("");
    }
    return strdup(value);
}

static const char *voice_tts_infer_transport(const char *base_url)
{
    if (base_url == NULL || base_url[0] == '\0') {
        return "";
    }
    if (strncmp(base_url, "wss://", 6) == 0 || strncmp(base_url, "ws://", 5) == 0) {
        return "websocket";
    }
    if (strncmp(base_url, "https://", 8) == 0 || strncmp(base_url, "http://", 7) == 0) {
        return "http";
    }
    return "";
}

static esp_err_t voice_build_url(const voice_gateway_client_t *client,
                                 const char *path,
                                 const char *query,
                                 char *out,
                                 size_t out_size)
{
    if (client == NULL || client->base_url == NULL || path == NULL || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t base_len = strlen(client->base_url);
    bool base_has_slash = (base_len > 0 && client->base_url[base_len - 1] == '/');
    bool path_has_slash = (path[0] == '/');

    int n = 0;
    if (query != NULL && query[0] != '\0') {
        n = snprintf(out,
                     out_size,
                     "%s%s%s%s%s",
                     client->base_url,
                     (base_has_slash || path_has_slash) ? "" : "/",
                     (base_has_slash && path_has_slash) ? (path + 1) : path,
                     (query[0] == '?') ? "" : "?",
                     query);
    } else {
        n = snprintf(out,
                     out_size,
                     "%s%s%s",
                     client->base_url,
                     (base_has_slash || path_has_slash) ? "" : "/",
                     (base_has_slash && path_has_slash) ? (path + 1) : path);
    }

    if (n < 0 || (size_t)n >= out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t voice_http_post(voice_gateway_client_t *client,
                                 const char *url,
                                 const uint8_t *payload,
                                 int payload_len,
                                 const char *content_type,
                                 uint8_t **resp_data,
                                 int *resp_len,
                                 int *http_status)
{
    if (client == NULL || url == NULL || resp_data == NULL || resp_len == NULL || http_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *resp_data = NULL;
    *resp_len = 0;
    *http_status = 0;

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (client->timeout_ms > 0) ? client->timeout_ms : VOICE_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };

    esp_http_client_handle_t http_client = esp_http_client_init(&http_cfg);
    if (http_client == NULL) {
        return ESP_FAIL;
    }

    if (content_type != NULL && content_type[0] != '\0') {
        esp_http_client_set_header(http_client, "Content-Type", content_type);
    }
    if (client->access_token != NULL && client->access_token[0] != '\0') {
        char auth_header[256] = {0};
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", client->access_token);
        esp_http_client_set_header(http_client, "Authorization", auth_header);
    }

    if (payload != NULL && payload_len > 0) {
        esp_http_client_set_post_field(http_client, (const char *)payload, payload_len);
    } else {
        esp_http_client_set_post_field(http_client, "", 0);
    }

    esp_err_t err = esp_http_client_perform(http_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(http_client);
        return err;
    }

    *http_status = esp_http_client_get_status_code(http_client);

    int cap = esp_http_client_get_content_length(http_client);
    if (cap <= 0) {
        cap = VOICE_HTTP_RX_INIT_CAP;
    }
    if (cap > VOICE_HTTP_RX_MAX_CAP) {
        cap = VOICE_HTTP_RX_MAX_CAP;
    }

    uint8_t *body = (uint8_t *)malloc((size_t)cap + 1);
    if (body == NULL) {
        esp_http_client_cleanup(http_client);
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    while (1) {
        int room = cap - total;
        if (room == 0) {
            if (cap >= VOICE_HTTP_RX_MAX_CAP) {
                free(body);
                esp_http_client_cleanup(http_client);
                return ESP_ERR_NO_MEM;
            }
            int next_cap = cap * 2;
            if (next_cap > VOICE_HTTP_RX_MAX_CAP) {
                next_cap = VOICE_HTTP_RX_MAX_CAP;
            }
            uint8_t *new_body = (uint8_t *)realloc(body, (size_t)next_cap + 1);
            if (new_body == NULL) {
                free(body);
                esp_http_client_cleanup(http_client);
                return ESP_ERR_NO_MEM;
            }
            body = new_body;
            cap = next_cap;
            room = cap - total;
        }

        int read_len = esp_http_client_read(http_client, (char *)body + total, room);
        if (read_len < 0) {
            free(body);
            esp_http_client_cleanup(http_client);
            return ESP_FAIL;
        }
        if (read_len == 0) {
            break;
        }
        total += read_len;
    }

    body[total] = '\0';
    *resp_data = body;
    *resp_len = total;

    esp_http_client_cleanup(http_client);
    return ESP_OK;
}

static esp_err_t voice_parse_text_from_json(const uint8_t *json_data,
                                            int json_len,
                                            char *out_text,
                                            int out_text_size)
{
    if (out_text == NULL || out_text_size <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out_text[0] = '\0';
    if (json_data == NULL || json_len <= 0) {
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength((const char *)json_data, json_len);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *text = cJSON_GetObjectItem(root, "text");
    if (text == NULL || !cJSON_IsString(text)) {
        const cJSON *result = cJSON_GetObjectItem(root, "result");
        if (result != NULL && cJSON_IsObject(result)) {
            text = cJSON_GetObjectItem(result, "text");
        }
    }

    if (text != NULL && cJSON_IsString(text) && text->valuestring != NULL) {
        strncpy(out_text, text->valuestring, out_text_size - 1);
        out_text[out_text_size - 1] = '\0';
    }

    cJSON_Delete(root);
    return ESP_OK;
}

voice_gateway_client_t *voice_gateway_client_create(const voice_gateway_client_cfg_t *cfg)
{
    if (cfg == NULL || cfg->base_url == NULL || cfg->base_url[0] == '\0') {
        return NULL;
    }

    voice_gateway_client_t *client = (voice_gateway_client_t *)calloc(1, sizeof(voice_gateway_client_t));
    if (client == NULL) {
        return NULL;
    }

    client->base_url = strdup(cfg->base_url);
    client->access_token = voice_strdup_or_empty(cfg->access_token);
    client->timeout_ms = cfg->timeout_ms;

    client->stt_provider = voice_strdup_or_empty(cfg->stt_provider);
    client->stt_model_name = voice_strdup_or_empty(cfg->stt_model_name);
    client->stt_api_key = voice_strdup_or_empty(cfg->stt_api_key);
    client->stt_app_id = voice_strdup_or_empty(cfg->stt_app_id);
    client->stt_access_token = voice_strdup_or_empty(cfg->stt_access_token);
    client->stt_secret_key = voice_strdup_or_empty(cfg->stt_secret_key);
    client->stt_base_url = voice_strdup_or_empty(cfg->stt_base_url);
    client->stt_language = voice_strdup_or_empty(cfg->stt_language);
    client->stt_enable_itn = cfg->stt_enable_itn;
    client->stt_enable_punc = cfg->stt_enable_punc;
    client->stt_enable_nonstream = cfg->stt_enable_nonstream;
    client->stt_result_type = voice_strdup_or_empty(cfg->stt_result_type);
    client->stt_end_window_size_ms = cfg->stt_end_window_size_ms;

    client->tts_provider = voice_strdup_or_empty(cfg->tts_provider);
    client->tts_model_name = voice_strdup_or_empty(cfg->tts_model_name);
    client->tts_api_key = voice_strdup_or_empty(cfg->tts_api_key);
    client->tts_app_id = voice_strdup_or_empty(cfg->tts_app_id);
    client->tts_access_token = voice_strdup_or_empty(cfg->tts_access_token);
    client->tts_secret_key = voice_strdup_or_empty(cfg->tts_secret_key);
    client->tts_base_url = voice_strdup_or_empty(cfg->tts_base_url);
    const char *tts_transport = cfg->tts_transport;
    if ((tts_transport == NULL || tts_transport[0] == '\0') &&
        cfg->tts_base_url != NULL && cfg->tts_base_url[0] != '\0') {
        tts_transport = voice_tts_infer_transport(cfg->tts_base_url);
    }
    client->tts_transport = voice_strdup_or_empty(tts_transport);

    if (client->base_url == NULL || client->access_token == NULL ||
        client->stt_provider == NULL || client->stt_model_name == NULL ||
        client->stt_api_key == NULL || client->stt_app_id == NULL ||
        client->stt_access_token == NULL || client->stt_secret_key == NULL ||
        client->stt_base_url == NULL ||
        client->stt_language == NULL || client->stt_result_type == NULL ||
        client->tts_provider == NULL || client->tts_model_name == NULL ||
        client->tts_api_key == NULL || client->tts_app_id == NULL ||
        client->tts_access_token == NULL || client->tts_secret_key == NULL ||
        client->tts_base_url == NULL || client->tts_transport == NULL) {
        free(client->base_url);
        free(client->access_token);
        free(client->stt_provider);
        free(client->stt_model_name);
        free(client->stt_api_key);
        free(client->stt_app_id);
        free(client->stt_access_token);
        free(client->stt_secret_key);
        free(client->stt_base_url);
        free(client->stt_language);
        free(client->stt_result_type);
        free(client->tts_provider);
        free(client->tts_model_name);
        free(client->tts_api_key);
        free(client->tts_app_id);
        free(client->tts_access_token);
        free(client->tts_secret_key);
        free(client->tts_base_url);
        free(client->tts_transport);
        free(client);
        return NULL;
    }

    ESP_LOGI(TAG,
             "gateway client created: %s, stt_provider=%s, stt_model=%s",
             client->base_url,
             client->stt_provider,
             client->stt_model_name);
    return client;
}

void voice_gateway_client_destroy(voice_gateway_client_t *client)
{
    if (client == NULL) {
        return;
    }

    free(client->base_url);
    free(client->access_token);
    free(client->stt_provider);
    free(client->stt_model_name);
    free(client->stt_api_key);
    free(client->stt_app_id);
    free(client->stt_access_token);
    free(client->stt_secret_key);
    free(client->stt_base_url);
    free(client->stt_language);
    free(client->stt_result_type);
    free(client->tts_provider);
    free(client->tts_model_name);
    free(client->tts_api_key);
    free(client->tts_app_id);
    free(client->tts_access_token);
    free(client->tts_secret_key);
    free(client->tts_base_url);
    free(client->tts_transport);
    free(client);
}

esp_err_t voice_gateway_stt_start(voice_gateway_client_t *client,
                                  const char *session_id,
                                  int sample_rate_hz)
{
    if (client == NULL || session_id == NULL || sample_rate_hz <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[256] = {0};
    esp_err_t err = voice_build_url(client, VOICE_STT_START_PATH, NULL, url, sizeof(url));
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Legacy flat fields kept for compatibility with existing gateway handlers. */
    cJSON_AddStringToObject(root, "session_id", session_id);
    cJSON_AddStringToObject(root, "format", "pcm_s16le");
    cJSON_AddNumberToObject(root, "sample_rate", sample_rate_hz);
    cJSON_AddNumberToObject(root, "channels", 1);
    if (client->stt_provider[0] != '\0') {
        cJSON_AddStringToObject(root, "provider", client->stt_provider);
    }
    if (client->stt_model_name[0] != '\0') {
        cJSON_AddStringToObject(root, "model_name", client->stt_model_name);
    }
    if (client->stt_api_key[0] != '\0') {
        cJSON_AddStringToObject(root, "provider_api_key", client->stt_api_key);
    }
    if (client->stt_app_id[0] != '\0') {
        cJSON_AddStringToObject(root, "provider_app_id", client->stt_app_id);
        cJSON_AddStringToObject(root, "app_id", client->stt_app_id);
    }
    if (client->stt_access_token[0] != '\0') {
        cJSON_AddStringToObject(root, "provider_access_token", client->stt_access_token);
        cJSON_AddStringToObject(root, "access_token", client->stt_access_token);
    }
    if (client->stt_secret_key[0] != '\0') {
        cJSON_AddStringToObject(root, "provider_secret_key", client->stt_secret_key);
        cJSON_AddStringToObject(root, "secret_key", client->stt_secret_key);
    }
    if (client->stt_base_url[0] != '\0') {
        cJSON_AddStringToObject(root, "provider_base_url", client->stt_base_url);
    }
    if (client->stt_language[0] != '\0') {
        cJSON_AddStringToObject(root, "language", client->stt_language);
    }
    cJSON_AddBoolToObject(root, "enable_itn", client->stt_enable_itn);
    cJSON_AddBoolToObject(root, "enable_punc", client->stt_enable_punc);
    if (client->stt_enable_nonstream) {
        cJSON_AddBoolToObject(root, "enable_nonstream", true);
    }
    if (client->stt_result_type[0] != '\0') {
        cJSON_AddStringToObject(root, "result_type", client->stt_result_type);
    }
    if (client->stt_end_window_size_ms >= 200) {
        cJSON_AddNumberToObject(root, "end_window_size", client->stt_end_window_size_ms);
    }

    /* Volcengine schema-compatible nested fields for gateway direct pass-through. */
    cJSON *audio = cJSON_CreateObject();
    cJSON *request = cJSON_CreateObject();
    if (audio == NULL || request == NULL) {
        cJSON_Delete(audio);
        cJSON_Delete(request);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(audio, "format", "pcm");
    cJSON_AddStringToObject(audio, "codec", "raw");
    cJSON_AddNumberToObject(audio, "rate", sample_rate_hz);
    cJSON_AddNumberToObject(audio, "bits", 16);
    cJSON_AddNumberToObject(audio, "channel", 1);
    if (client->stt_language[0] != '\0') {
        cJSON_AddStringToObject(audio, "language", client->stt_language);
    }
    cJSON_AddItemToObject(root, "audio", audio);

    cJSON_AddStringToObject(request,
                            "model_name",
                            (client->stt_model_name[0] != '\0') ? client->stt_model_name : "bigmodel");
    cJSON_AddBoolToObject(request, "enable_itn", client->stt_enable_itn);
    cJSON_AddBoolToObject(request, "enable_punc", client->stt_enable_punc);
    if (client->stt_enable_nonstream) {
        cJSON_AddBoolToObject(request, "enable_nonstream", true);
    }
    if (client->stt_result_type[0] != '\0') {
        cJSON_AddStringToObject(request, "result_type", client->stt_result_type);
    }
    if (client->stt_end_window_size_ms >= 200) {
        cJSON_AddNumberToObject(request, "end_window_size", client->stt_end_window_size_ms);
    }
    if (client->stt_app_id[0] != '\0') {
        cJSON_AddStringToObject(request, "app_id", client->stt_app_id);
    }
    if (client->stt_access_token[0] != '\0') {
        cJSON_AddStringToObject(request, "access_token", client->stt_access_token);
    }
    if (client->stt_secret_key[0] != '\0') {
        cJSON_AddStringToObject(request, "secret_key", client->stt_secret_key);
    }
    cJSON_AddItemToObject(root, "request", request);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t *resp = NULL;
    int resp_len = 0;
    int http_status = 0;
    err = voice_http_post(client,
                          url,
                          (const uint8_t *)payload,
                          (int)strlen(payload),
                          "application/json",
                          &resp,
                          &resp_len,
                          &http_status);
    free(payload);

    if (err != ESP_OK) {
        free(resp);
        return err;
    }

    if (http_status < 200 || http_status >= 300) {
        ESP_LOGW(TAG, "stt start failed: http=%d body=%.*s", http_status, resp_len, (const char *)resp);
        free(resp);
        return ESP_FAIL;
    }

    free(resp);
    return ESP_OK;
}

esp_err_t voice_gateway_stt_send_audio(voice_gateway_client_t *client,
                                       const char *session_id,
                                       const uint8_t *pcm,
                                       int len)
{
    if (client == NULL || session_id == NULL || pcm == NULL || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char query[160] = {0};
    snprintf(query, sizeof(query), "session_id=%s", session_id);

    char url[320] = {0};
    esp_err_t err = voice_build_url(client, VOICE_STT_CHUNK_PATH, query, url, sizeof(url));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t *resp = NULL;
    int resp_len = 0;
    int http_status = 0;
    err = voice_http_post(client,
                          url,
                          pcm,
                          len,
                          "application/octet-stream",
                          &resp,
                          &resp_len,
                          &http_status);
    if (err != ESP_OK) {
        free(resp);
        return err;
    }

    if (http_status < 200 || http_status >= 300) {
        ESP_LOGW(TAG, "stt chunk failed: http=%d body=%.*s", http_status, resp_len, (const char *)resp);
        free(resp);
        return ESP_FAIL;
    }

    free(resp);
    return ESP_OK;
}

esp_err_t voice_gateway_stt_stop(voice_gateway_client_t *client,
                                 const char *session_id,
                                 char *out_text,
                                 int out_text_size)
{
    if (client == NULL || session_id == NULL || out_text == NULL || out_text_size <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out_text[0] = '\0';

    char url[256] = {0};
    esp_err_t err = voice_build_url(client, VOICE_STT_STOP_PATH, NULL, url, sizeof(url));
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t *resp = NULL;
    int resp_len = 0;
    int http_status = 0;
    err = voice_http_post(client,
                          url,
                          (const uint8_t *)payload,
                          (int)strlen(payload),
                          "application/json",
                          &resp,
                          &resp_len,
                          &http_status);
    free(payload);

    if (err != ESP_OK) {
        free(resp);
        return err;
    }

    if (http_status < 200 || http_status >= 300) {
        ESP_LOGW(TAG, "stt stop failed: http=%d body=%.*s", http_status, resp_len, (const char *)resp);
        free(resp);
        return ESP_FAIL;
    }

    err = voice_parse_text_from_json(resp, resp_len, out_text, out_text_size);
    free(resp);
    return err;
}

esp_err_t voice_gateway_tts_synthesize(voice_gateway_client_t *client,
                                       const char *session_id,
                                       const char *text,
                                       const char *voice_name,
                                       uint8_t **audio_data,
                                       int *audio_len)
{
    if (client == NULL || session_id == NULL || text == NULL ||
        audio_data == NULL || audio_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *audio_data = NULL;
    *audio_len = 0;

    char url[256] = {0};
    esp_err_t err = voice_build_url(client, VOICE_TTS_PATH, NULL, url, sizeof(url));
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "session_id", session_id);
    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddStringToObject(root, "format", "pcm_s16le");
    cJSON_AddNumberToObject(root, "sample_rate", 16000);
    if (client->tts_provider[0] != '\0') {
        cJSON_AddStringToObject(root, "provider", client->tts_provider);
    }
    if (client->tts_model_name[0] != '\0') {
        cJSON_AddStringToObject(root, "model_name", client->tts_model_name);
    }
    if (client->tts_api_key[0] != '\0') {
        cJSON_AddStringToObject(root, "provider_api_key", client->tts_api_key);
    }
    if (client->tts_app_id[0] != '\0') {
        cJSON_AddStringToObject(root, "provider_app_id", client->tts_app_id);
        cJSON_AddStringToObject(root, "app_id", client->tts_app_id);
    }
    if (client->tts_access_token[0] != '\0') {
        cJSON_AddStringToObject(root, "provider_access_token", client->tts_access_token);
        cJSON_AddStringToObject(root, "access_token", client->tts_access_token);
    }
    if (client->tts_secret_key[0] != '\0') {
        cJSON_AddStringToObject(root, "provider_secret_key", client->tts_secret_key);
        cJSON_AddStringToObject(root, "secret_key", client->tts_secret_key);
    }
    if (client->tts_base_url[0] != '\0') {
        cJSON_AddStringToObject(root, "provider_base_url", client->tts_base_url);
    }
    if (client->tts_transport[0] != '\0') {
        cJSON_AddStringToObject(root, "provider_transport", client->tts_transport);
        cJSON_AddStringToObject(root, "transport", client->tts_transport);
        if (strcmp(client->tts_transport, "websocket") == 0) {
            cJSON_AddStringToObject(root, "operation", "submit");
        }
    }
    if (voice_name != NULL && voice_name[0] != '\0') {
        cJSON_AddStringToObject(root, "voice", voice_name);
    }
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t *resp = NULL;
    int resp_len = 0;
    int http_status = 0;
    err = voice_http_post(client,
                          url,
                          (const uint8_t *)payload,
                          (int)strlen(payload),
                          "application/json",
                          &resp,
                          &resp_len,
                          &http_status);
    free(payload);

    if (err != ESP_OK) {
        free(resp);
        return err;
    }

    if (http_status < 200 || http_status >= 300) {
        ESP_LOGW(TAG, "tts synth failed: http=%d body=%.*s", http_status, resp_len, (const char *)resp);
        free(resp);
        return ESP_FAIL;
    }

    *audio_data = resp;
    *audio_len = resp_len;
    return ESP_OK;
}
