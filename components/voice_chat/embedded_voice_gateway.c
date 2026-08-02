#include "embedded_voice_gateway.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "embedded_voice_gw";

#define GW_CONNECTED_BIT  BIT0
#define GW_STARTED_BIT    BIT1
#define GW_FINISHED_BIT   BIT2
#define GW_ERROR_BIT      BIT3
#define GW_WAIT_BITS      (GW_CONNECTED_BIT | GW_STARTED_BIT | GW_FINISHED_BIT | GW_ERROR_BIT)
#define GW_JSON_MAX       4096
#define GW_AUDIO_MAX      (2 * 1024 * 1024)

typedef enum {
    GW_TASK_NONE = 0,
    GW_TASK_STT,
    GW_TASK_TTS,
} gw_task_type_t;

typedef struct {
    embedded_voice_gateway_t *gateway;
    gw_task_type_t type;
    esp_websocket_client_handle_t ws;
    EventGroupHandle_t events;
    char task_id[33];
    char *json;
    int json_cap;
    char stt_text[1024];
    uint8_t *audio;
    int audio_len;
    int audio_cap;
    esp_err_t error;
} gw_ws_session_t;

struct embedded_voice_gateway {
    char *api_key;
    char *websocket_url;
    char *stt_model;
    char *tts_model;
    char *tts_voice;
    int timeout_ms;
    gw_ws_session_t *stt;
};

static char *gw_strdup_default(const char *value, const char *fallback)
{
    return strdup((value != NULL && value[0] != '\0') ? value : fallback);
}

static void gw_make_task_id(char out[33])
{
    for (int i = 0; i < 4; i++) {
        snprintf(out + i * 8, 9, "%08lx", (unsigned long)esp_random());
    }
    out[32] = '\0';
}

static TickType_t gw_timeout_ticks(const embedded_voice_gateway_t *gateway)
{
    int timeout_ms = (gateway != NULL && gateway->timeout_ms > 0)
                         ? gateway->timeout_ms
                         : 15000;
    return pdMS_TO_TICKS(timeout_ms);
}

static bool gw_append_audio(gw_ws_session_t *session, const uint8_t *data, int len)
{
    if (len <= 0) {
        return true;
    }
    if (session->audio_len + len > GW_AUDIO_MAX) {
        return false;
    }
    if (session->audio_len + len > session->audio_cap) {
        int next = session->audio_cap > 0 ? session->audio_cap : 8192;
        while (next < session->audio_len + len && next < GW_AUDIO_MAX) {
            next *= 2;
        }
        if (next > GW_AUDIO_MAX) {
            next = GW_AUDIO_MAX;
        }
        uint8_t *grown = realloc(session->audio, (size_t)next);
        if (grown == NULL) {
            return false;
        }
        session->audio = grown;
        session->audio_cap = next;
    }
    memcpy(session->audio + session->audio_len, data, (size_t)len);
    session->audio_len += len;
    return true;
}

static void gw_parse_json_event(gw_ws_session_t *session, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "Ignoring invalid upstream JSON");
        return;
    }

    cJSON *header = cJSON_GetObjectItemCaseSensitive(root, "header");
    cJSON *event = cJSON_GetObjectItemCaseSensitive(header, "event");
    const char *event_name = cJSON_IsString(event) ? event->valuestring : "";
    ESP_LOGI(TAG, "%s upstream event=%s",
             session->type == GW_TASK_TTS ? "TTS" : "ASR",
             event_name[0] != '\0' ? event_name : "(missing)");

    if (strcmp(event_name, "task-started") == 0) {
        xEventGroupSetBits(session->events, GW_STARTED_BIT);
    } else if (strcmp(event_name, "result-generated") == 0 && session->type == GW_TASK_STT) {
        cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        cJSON *output = cJSON_GetObjectItemCaseSensitive(payload, "output");
        cJSON *sentence = cJSON_GetObjectItemCaseSensitive(output, "sentence");
        cJSON *text = cJSON_GetObjectItemCaseSensitive(sentence, "text");
        if (cJSON_IsString(text) && text->valuestring[0] != '\0') {
            strlcpy(session->stt_text, text->valuestring, sizeof(session->stt_text));
        }
    } else if (strcmp(event_name, "task-finished") == 0) {
        xEventGroupSetBits(session->events, GW_FINISHED_BIT);
    } else if (strcmp(event_name, "task-failed") == 0) {
        cJSON *message = cJSON_GetObjectItemCaseSensitive(header, "error_message");
        ESP_LOGE(TAG, "Upstream task failed: %s",
                 cJSON_IsString(message) ? message->valuestring : "unknown error");
        session->error = ESP_FAIL;
        xEventGroupSetBits(session->events, GW_ERROR_BIT);
    }
    cJSON_Delete(root);
}

static void gw_ws_event_handler(void *arg,
                                esp_event_base_t base,
                                int32_t event_id,
                                void *event_data)
{
    (void)base;
    gw_ws_session_t *session = (gw_ws_session_t *)arg;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    if (session == NULL) {
        return;
    }

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            xEventGroupSetBits(session->events, GW_CONNECTED_BIT);
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data == NULL) {
                break;
            }
            if (data->op_code == 0x2 && session->type == GW_TASK_TTS) {
                if (!gw_append_audio(session, (const uint8_t *)data->data_ptr, data->data_len)) {
                    ESP_LOGE(TAG, "TTS audio exceeds buffer limit (%d bytes), received=%d",
                             GW_AUDIO_MAX, session->audio_len);
                    session->error = ESP_ERR_NO_MEM;
                    xEventGroupSetBits(session->events, GW_ERROR_BIT);
                } else if (session->audio_len == data->data_len ||
                           (session->audio_len % (64 * 1024)) < data->data_len) {
                    ESP_LOGI(TAG, "TTS audio received=%d bytes", session->audio_len);
                }
                break;
            }
            if ((data->op_code == 0x1 || data->op_code == 0x0) &&
                data->payload_len > 0 && data->payload_len <= GW_JSON_MAX &&
                data->payload_offset >= 0 &&
                data->payload_offset + data->data_len <= data->payload_len) {
                if (session->json_cap < data->payload_len + 1) {
                    char *grown = realloc(session->json, (size_t)data->payload_len + 1);
                    if (grown == NULL) {
                        session->error = ESP_ERR_NO_MEM;
                        xEventGroupSetBits(session->events, GW_ERROR_BIT);
                        break;
                    }
                    session->json = grown;
                    session->json_cap = data->payload_len + 1;
                }
                memcpy(session->json + data->payload_offset, data->data_ptr, (size_t)data->data_len);
                if (data->payload_offset + data->data_len == data->payload_len) {
                    session->json[data->payload_len] = '\0';
                    gw_parse_json_event(session, session->json);
                }
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            if (data != NULL) {
                ESP_LOGE(TAG,
                         "%s websocket error type=%d tls=%s stack=%d socket_errno=%d "
                         "http=%d close=%d audio=%d",
                         session->type == GW_TASK_TTS ? "TTS" : "ASR",
                         data->error_handle.error_type,
                         esp_err_to_name(data->error_handle.esp_tls_last_esp_err),
                         data->error_handle.esp_tls_stack_err,
                         data->error_handle.esp_transport_sock_errno,
                         data->error_handle.esp_ws_handshake_status_code,
                         data->close_status_code, session->audio_len);
            }
            if ((xEventGroupGetBits(session->events) & GW_FINISHED_BIT) == 0) {
                if (session->error == ESP_OK) {
                    session->error = ESP_ERR_HTTP_CONNECT;
                }
                xEventGroupSetBits(session->events, GW_ERROR_BIT);
            }
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            if (data != NULL) {
                ESP_LOGW(TAG, "%s websocket disconnected close=%d audio=%d",
                         session->type == GW_TASK_TTS ? "TTS" : "ASR",
                         data->close_status_code, session->audio_len);
            }
            if ((xEventGroupGetBits(session->events) & GW_FINISHED_BIT) == 0) {
                if (session->error == ESP_OK) {
                    session->error = ESP_ERR_HTTP_CONNECT;
                }
                xEventGroupSetBits(session->events, GW_ERROR_BIT);
            }
            break;
        default:
            break;
    }
}

static gw_ws_session_t *gw_session_open(embedded_voice_gateway_t *gateway, gw_task_type_t type)
{
    gw_ws_session_t *session = calloc(1, sizeof(*session));
    if (session == NULL) {
        return NULL;
    }
    session->gateway = gateway;
    session->type = type;
    session->error = ESP_OK;
    session->events = xEventGroupCreate();
    gw_make_task_id(session->task_id);
    if (session->events == NULL) {
        free(session);
        return NULL;
    }

    size_t header_len = strlen(gateway->api_key) + 32;
    char *headers = malloc(header_len);
    if (headers == NULL) {
        vEventGroupDelete(session->events);
        free(session);
        return NULL;
    }
    snprintf(headers, header_len, "Authorization: bearer %s\r\n", gateway->api_key);

    esp_websocket_client_config_t cfg = {
        .uri = gateway->websocket_url,
        .disable_auto_reconnect = true,
        .task_stack = 6144,
        .buffer_size = 4096,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .network_timeout_ms = gateway->timeout_ms,
        .user_context = session,
    };
    session->ws = esp_websocket_client_init(&cfg);
    free(headers);
    if (session->ws == NULL ||
        esp_websocket_register_events(session->ws, WEBSOCKET_EVENT_ANY,
                                      gw_ws_event_handler, session) != ESP_OK ||
        esp_websocket_client_start(session->ws) != ESP_OK) {
        if (session->ws != NULL) {
            esp_websocket_client_destroy(session->ws);
        }
        vEventGroupDelete(session->events);
        free(session);
        return NULL;
    }

    EventBits_t bits = xEventGroupWaitBits(session->events,
                                           GW_CONNECTED_BIT | GW_ERROR_BIT,
                                           pdFALSE, pdFALSE,
                                           gw_timeout_ticks(gateway));
    if ((bits & GW_CONNECTED_BIT) == 0) {
        session->error = (bits & GW_ERROR_BIT) ? session->error : ESP_ERR_TIMEOUT;
        return session;
    }
    return session;
}

static void gw_session_close(gw_ws_session_t *session)
{
    if (session == NULL) {
        return;
    }
    if (session->ws != NULL) {
        (void)esp_websocket_client_stop(session->ws);
        (void)esp_websocket_unregister_events(session->ws, WEBSOCKET_EVENT_ANY,
                                              gw_ws_event_handler);
        (void)esp_websocket_client_destroy(session->ws);
    }
    if (session->events != NULL) {
        vEventGroupDelete(session->events);
    }
    free(session->json);
    free(session->audio);
    free(session);
}

static esp_err_t gw_send_text(gw_ws_session_t *session, const char *text)
{
    int len = (int)strlen(text);
    return esp_websocket_client_send_text(session->ws, text, len,
                                          gw_timeout_ticks(session->gateway)) == len
               ? ESP_OK
               : ESP_FAIL;
}

static esp_err_t gw_wait_started(gw_ws_session_t *session)
{
    EventBits_t bits = xEventGroupWaitBits(session->events,
                                           GW_STARTED_BIT | GW_ERROR_BIT,
                                           pdFALSE, pdFALSE,
                                           gw_timeout_ticks(session->gateway));
    if (bits & GW_STARTED_BIT) {
        return ESP_OK;
    }
    return (bits & GW_ERROR_BIT) ? session->error : ESP_ERR_TIMEOUT;
}

static esp_err_t gw_wait_finished(gw_ws_session_t *session)
{
    EventBits_t bits = xEventGroupWaitBits(session->events,
                                           GW_FINISHED_BIT | GW_ERROR_BIT,
                                           pdFALSE, pdFALSE,
                                           gw_timeout_ticks(session->gateway));
    if (bits & GW_FINISHED_BIT) {
        return ESP_OK;
    }
    return (bits & GW_ERROR_BIT) ? session->error : ESP_ERR_TIMEOUT;
}

embedded_voice_gateway_t *embedded_voice_gateway_create(const embedded_voice_gateway_cfg_t *cfg)
{
    if (cfg == NULL || cfg->api_key == NULL || cfg->api_key[0] == '\0') {
        return NULL;
    }
    embedded_voice_gateway_t *gateway = calloc(1, sizeof(*gateway));
    if (gateway == NULL) {
        return NULL;
    }
    gateway->api_key = strdup(cfg->api_key);
    gateway->websocket_url = gw_strdup_default(
        cfg->websocket_url, "wss://dashscope.aliyuncs.com/api-ws/v1/inference/");
    gateway->stt_model = gw_strdup_default(cfg->stt_model, "fun-asr-realtime");
    gateway->tts_model = gw_strdup_default(cfg->tts_model, "cosyvoice-v3-flash");
    gateway->tts_voice = gw_strdup_default(cfg->tts_voice, "longanyang");
    gateway->timeout_ms = cfg->timeout_ms > 0 ? cfg->timeout_ms : 15000;
    if (gateway->api_key == NULL || gateway->websocket_url == NULL ||
        gateway->stt_model == NULL || gateway->tts_model == NULL ||
        gateway->tts_voice == NULL) {
        embedded_voice_gateway_destroy(gateway);
        return NULL;
    }
    return gateway;
}

void embedded_voice_gateway_destroy(embedded_voice_gateway_t *gateway)
{
    if (gateway == NULL) {
        return;
    }
    gw_session_close(gateway->stt);
    free(gateway->api_key);
    free(gateway->websocket_url);
    free(gateway->stt_model);
    free(gateway->tts_model);
    free(gateway->tts_voice);
    free(gateway);
}

esp_err_t embedded_voice_gateway_stt_start(embedded_voice_gateway_t *gateway,
                                           const char *session_id,
                                           int sample_rate_hz)
{
    (void)session_id;
    if (gateway == NULL || gateway->stt != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    gw_ws_session_t *session = gw_session_open(gateway, GW_TASK_STT);
    if (session == NULL) {
        return ESP_ERR_NO_MEM;
    }
    gateway->stt = session;
    if (session->error != ESP_OK) {
        return session->error;
    }

    char request[768];
    int n = snprintf(
        request, sizeof(request),
        "{\"header\":{\"action\":\"run-task\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},"
        "\"payload\":{\"task_group\":\"audio\",\"task\":\"asr\",\"function\":\"recognition\","
        "\"model\":\"%s\",\"parameters\":{\"sample_rate\":%d,\"format\":\"pcm\"},\"input\":{}}}",
        session->task_id, gateway->stt_model, sample_rate_hz);
    if (n <= 0 || n >= (int)sizeof(request)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = gw_send_text(session, request);
    if (err == ESP_OK) {
        err = gw_wait_started(session);
    }
    if (err != ESP_OK) {
        gateway->stt = NULL;
        gw_session_close(session);
    }
    return err;
}

esp_err_t embedded_voice_gateway_stt_send_audio(embedded_voice_gateway_t *gateway,
                                                const uint8_t *pcm,
                                                int len)
{
    if (gateway == NULL || gateway->stt == NULL || pcm == NULL || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int sent = esp_websocket_client_send_bin(gateway->stt->ws, (const char *)pcm, len,
                                             gw_timeout_ticks(gateway));
    return sent == len ? ESP_OK : ESP_FAIL;
}

esp_err_t embedded_voice_gateway_stt_stop(embedded_voice_gateway_t *gateway,
                                          char *out_text,
                                          int out_text_size)
{
    if (gateway == NULL || gateway->stt == NULL || out_text == NULL || out_text_size <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    gw_ws_session_t *session = gateway->stt;
    char request[192];
    snprintf(request, sizeof(request),
             "{\"header\":{\"action\":\"finish-task\",\"task_id\":\"%s\","
             "\"streaming\":\"duplex\"},\"payload\":{\"input\":{}}}",
             session->task_id);
    esp_err_t err = gw_send_text(session, request);
    if (err == ESP_OK) {
        err = gw_wait_finished(session);
    }
    if (err == ESP_OK) {
        strlcpy(out_text, session->stt_text, (size_t)out_text_size);
    }
    gateway->stt = NULL;
    gw_session_close(session);
    return err;
}

esp_err_t embedded_voice_gateway_tts(embedded_voice_gateway_t *gateway,
                                     const char *session_id,
                                     const char *text,
                                     const char *voice_name,
                                     uint8_t **audio_data,
                                     int *audio_len)
{
    (void)session_id;
    if (gateway == NULL || text == NULL || text[0] == '\0' ||
        audio_data == NULL || audio_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *audio_data = NULL;
    *audio_len = 0;
    gw_ws_session_t *session = gw_session_open(gateway, GW_TASK_TTS);
    if (session == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = session->error;
    const char *voice = (voice_name != NULL && voice_name[0] != '\0')
                            ? voice_name : gateway->tts_voice;

    char run[896];
    int n = snprintf(
        run, sizeof(run),
        "{\"header\":{\"action\":\"run-task\",\"task_id\":\"%s\",\"streaming\":\"duplex\"},"
        "\"payload\":{\"task_group\":\"audio\",\"task\":\"tts\",\"function\":\"SpeechSynthesizer\","
        "\"model\":\"%s\",\"parameters\":{\"text_type\":\"PlainText\",\"voice\":\"%s\","
        "\"format\":\"pcm\",\"sample_rate\":16000,\"volume\":50,\"rate\":1,\"pitch\":1,"
        "\"enable_ssml\":false},\"input\":{}}}",
        session->task_id, gateway->tts_model, voice);
    if (err == ESP_OK && (n <= 0 || n >= (int)sizeof(run))) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        err = gw_send_text(session, run);
    }
    if (err == ESP_OK) {
        err = gw_wait_started(session);
    }
    if (err == ESP_OK) {
        cJSON *root = cJSON_CreateObject();
        cJSON *header = cJSON_AddObjectToObject(root, "header");
        cJSON_AddStringToObject(header, "action", "continue-task");
        cJSON_AddStringToObject(header, "task_id", session->task_id);
        cJSON_AddStringToObject(header, "streaming", "duplex");
        cJSON *payload = cJSON_AddObjectToObject(root, "payload");
        cJSON *input = cJSON_AddObjectToObject(payload, "input");
        cJSON_AddStringToObject(input, "text", text);
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (json == NULL) {
            err = ESP_ERR_NO_MEM;
        } else {
            err = gw_send_text(session, json);
            free(json);
        }
    }
    if (err == ESP_OK) {
        char finish[192];
        snprintf(finish, sizeof(finish),
                 "{\"header\":{\"action\":\"finish-task\",\"task_id\":\"%s\","
                 "\"streaming\":\"duplex\"},\"payload\":{\"input\":{}}}",
                 session->task_id);
        err = gw_send_text(session, finish);
    }
    if (err == ESP_OK) {
        err = gw_wait_finished(session);
    }
    if (err == ESP_OK && session->audio_len > 0) {
        *audio_data = session->audio;
        *audio_len = session->audio_len;
        session->audio = NULL;
    } else if (err == ESP_OK) {
        err = ESP_ERR_NOT_FOUND;
    }
    gw_session_close(session);
    return err;
}
