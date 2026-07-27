#include "app_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "config.h"
#include "ai_service.h"
#include "conversation.h"
#include "ui.h"
#include "audio.h"
#include "voice_session.h"
#include "voice_gateway_client.h"
#if CONFIG_SDCARD_ENABLED
#include "storage.h"
#endif

static const char *TAG = "app_runtime";

typedef struct {
    ai_service_t *ai_service;
    voice_gateway_client_t *voice_gateway_client;
    conversation_manager_t conversation;
    voice_session_t voice_session;
    bool ui_ready;
    bool is_processing;
    bool is_debug_mode;
    bool is_recording;
    bool debug_playback_busy;
    uint8_t *recorded_audio;
    int recorded_len;
    volatile bool debug_record_req;
    volatile bool debug_play_record_req;
    volatile bool debug_play_req;
    volatile bool voice_round_req;
    TaskHandle_t chat_worker_task;
#if CONFIG_ENABLE_LOCAL_OFFLINE_WAKEUP
    TaskHandle_t local_wakeup_task;
    TickType_t local_wakeup_last_trigger_tick;
#endif
    bool wakeup_armed;
    TickType_t wakeup_armed_tick;
#if CONFIG_SDCARD_ENABLED
    volatile bool debug_sdcard_req;
    volatile bool debug_test_req;
#endif
} app_runtime_state_t;

static app_runtime_state_t s_runtime = {0};

#define VOICE_CAPTURE_MS             (3500)
#define VOICE_STT_TEXT_MAX           (1024)
#define VOICE_SESSION_ID_MAX         (64)
#define PCM_S16LE_BYTES_PER_SAMPLE   (2)
#define STT_DEBUG_PCM_SNAPSHOT_MAX_BYTES (128 * 1024)
/* HTTPS/mbedtls needs far more stack than the default main task. */
#define APP_CHAT_WORKER_STACK_SIZE   (24 * 1024)

#ifndef CONFIG_ENABLE_VOICE_WAKEUP
#define CONFIG_ENABLE_VOICE_WAKEUP 0
#endif

#ifndef CONFIG_VOICE_WAKEUP_WORDS
#define CONFIG_VOICE_WAKEUP_WORDS "hey box,ok box,xiao zhi"
#endif

#ifndef CONFIG_VOICE_WAKEUP_WINDOW_MS
#define CONFIG_VOICE_WAKEUP_WINDOW_MS 8000
#endif

#ifndef CONFIG_ENABLE_LOCAL_OFFLINE_WAKEUP
#define CONFIG_ENABLE_LOCAL_OFFLINE_WAKEUP 0
#endif

#ifndef CONFIG_LOCAL_WAKEUP_CHUNK_MS
#define CONFIG_LOCAL_WAKEUP_CHUNK_MS 80
#endif

#ifndef CONFIG_LOCAL_WAKEUP_PEAK_THRESHOLD
#define CONFIG_LOCAL_WAKEUP_PEAK_THRESHOLD 1800
#endif

#ifndef CONFIG_LOCAL_WAKEUP_SUSTAIN_MS
#define CONFIG_LOCAL_WAKEUP_SUSTAIN_MS 240
#endif

#ifndef CONFIG_LOCAL_WAKEUP_COOLDOWN_MS
#define CONFIG_LOCAL_WAKEUP_COOLDOWN_MS 3500
#endif

#if CONFIG_SDCARD_ENABLED
#define SD_MP3_PATH_MAX_LEN          (256)
#endif

#ifndef CONFIG_VOLCENGINE_STT_LANGUAGE
#define CONFIG_VOLCENGINE_STT_LANGUAGE "zh-CN"
#endif

#ifndef CONFIG_VOLCENGINE_STT_ENABLE_ITN
#define CONFIG_VOLCENGINE_STT_ENABLE_ITN 1
#endif

#ifndef CONFIG_VOLCENGINE_STT_ENABLE_PUNC
#define CONFIG_VOLCENGINE_STT_ENABLE_PUNC 1
#endif

#ifndef CONFIG_VOLCENGINE_STT_ENABLE_NONSTREAM
#define CONFIG_VOLCENGINE_STT_ENABLE_NONSTREAM 0
#endif

#ifndef CONFIG_VOLCENGINE_STT_RESULT_TYPE
#define CONFIG_VOLCENGINE_STT_RESULT_TYPE "full"
#endif

#ifndef CONFIG_VOLCENGINE_STT_END_WINDOW_SIZE_MS
#define CONFIG_VOLCENGINE_STT_END_WINDOW_SIZE_MS 800
#endif

typedef struct {
    TickType_t round_start_tick;
    TickType_t stt_start_tick;
    TickType_t capture_start_tick;
    TickType_t capture_end_tick;
    TickType_t stt_stop_start_tick;
    TickType_t stt_stop_end_tick;
    int sample_rate_hz;
    int requested_chunk_ms;
    int effective_chunk_ms;
    int requested_capture_ms;
    int effective_capture_ms;
    int chunk_bytes;
    int planned_chunks;
    int read_chunks;
    int sent_chunks;
    int empty_chunks;
    int total_pcm_bytes;
    int fail_chunk_index;
    bool stt_started;
    bool capture_started;
    uint8_t *pcm_snapshot;
    int pcm_snapshot_len;
    int pcm_snapshot_cap;
    char session_id[VOICE_SESSION_ID_MAX];
} stt_debug_trace_t;

static esp_err_t app_runtime_run_chat_turn(const char *user_text,
                                           char *assistant_text,
                                           size_t assistant_text_size);
static esp_err_t app_runtime_run_voice_chat_round(void);
static esp_err_t app_runtime_initialize_ai_service(void);
static esp_err_t app_runtime_initialize_voice_gateway_client(void);
#if CONFIG_ENABLE_LOCAL_OFFLINE_WAKEUP
static void app_local_wakeup_task(void *arg);
#endif

static const char *app_skip_text_spaces(const char *s)
{
    if (s == NULL) {
        return NULL;
    }

    while (*s != '\0' && isspace((unsigned char)*s)) {
        ++s;
    }
    return s;
}

#if CONFIG_ENABLE_VOICE_WAKEUP
static const char *app_consume_utf8_token(const char *s, const char *token)
{
    if (s == NULL || token == NULL) {
        return NULL;
    }

    size_t token_len = strlen(token);
    if (token_len == 0) {
        return s;
    }

    if (strncmp(s, token, token_len) == 0) {
        return s + token_len;
    }
    return NULL;
}

static const char *app_skip_wakeup_delimiters(const char *s)
{
    if (s == NULL) {
        return NULL;
    }

    while (*s != '\0') {
        if (isspace((unsigned char)*s) ||
            *s == ',' || *s == ':' || *s == '!' || *s == '?' ||
            *s == '.' || *s == ';') {
            ++s;
            continue;
        }

        const char *next = NULL;

        next = app_consume_utf8_token(s, "\xE3\x80\x80"); /* U+3000 ideographic space */
        if (next != NULL) {
            s = next;
            continue;
        }

        next = app_consume_utf8_token(s, "\xEF\xBC\x8C"); /* U+FF0C ， */
        if (next != NULL) {
            s = next;
            continue;
        }

        next = app_consume_utf8_token(s, "\xE3\x80\x82"); /* U+3002 。 */
        if (next != NULL) {
            s = next;
            continue;
        }

        next = app_consume_utf8_token(s, "\xEF\xBC\x81"); /* U+FF01 ！ */
        if (next != NULL) {
            s = next;
            continue;
        }

        next = app_consume_utf8_token(s, "\xEF\xBC\x9F"); /* U+FF1F ？ */
        if (next != NULL) {
            s = next;
            continue;
        }

        next = app_consume_utf8_token(s, "\xEF\xBC\x9A"); /* U+FF1A ： */
        if (next != NULL) {
            s = next;
            continue;
        }

        next = app_consume_utf8_token(s, "\xE3\x80\x81"); /* U+3001 、 */
        if (next != NULL) {
            s = next;
            continue;
        }

        next = app_consume_utf8_token(s, "\xEF\xBC\x9B"); /* U+FF1B ； */
        if (next != NULL) {
            s = next;
            continue;
        }

        break;
    }

    return s;
}

static size_t app_ascii_prefix_match_len_ci(const char *input, const char *phrase)
{
    if (input == NULL || phrase == NULL) {
        return 0;
    }

    const unsigned char *in = (const unsigned char *)input;
    const unsigned char *ph = (const unsigned char *)phrase;
    size_t matched_len = 0;

    while (*ph != '\0') {
        if (*in == '\0') {
            return 0;
        }

        if ((*in & 0x80U) != 0 || (*ph & 0x80U) != 0) {
            return 0;
        }

        if (tolower(*in) != tolower(*ph)) {
            return 0;
        }

        ++in;
        ++ph;
        ++matched_len;
    }

    return matched_len;
}

static size_t app_wakeup_prefix_match_len(const char *input, const char *phrase)
{
    if (input == NULL || phrase == NULL) {
        return 0;
    }

    size_t phrase_len = strlen(phrase);
    if (phrase_len == 0) {
        return 0;
    }

    if (strncmp(input, phrase, phrase_len) == 0) {
        return phrase_len;
    }

    return app_ascii_prefix_match_len_ci(input, phrase);
}

static void app_trim_ascii_trailing_spaces(char *s)
{
    if (s == NULL) {
        return;
    }

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        --len;
    }
}

static bool app_wakeup_match(const char *text,
                             const char **query_after_wakeup)
{
    if (text == NULL || query_after_wakeup == NULL) {
        return false;
    }

    const char *input = app_skip_wakeup_delimiters(text);
    if (input == NULL || *input == '\0') {
        return false;
    }

    char list_buf[256] = {0};
    strncpy(list_buf, CONFIG_VOICE_WAKEUP_WORDS, sizeof(list_buf) - 1);
    list_buf[sizeof(list_buf) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(list_buf, ",", &saveptr);
    while (token != NULL) {
        app_trim_ascii_trailing_spaces(token);
        const char *phrase = app_skip_text_spaces(token);

        if (phrase != NULL && phrase[0] != '\0') {
            size_t matched_len = app_wakeup_prefix_match_len(input, phrase);
            if (matched_len > 0) {
                const char *rest = app_skip_wakeup_delimiters(input + matched_len);
                *query_after_wakeup = (rest != NULL) ? rest : "";
                return true;
            }
        }

        token = strtok_r(NULL, ",", &saveptr);
    }

    return false;
}

static bool app_wakeup_window_expired(void)
{
    if (!s_runtime.wakeup_armed) {
        return true;
    }

    TickType_t now = xTaskGetTickCount();
    uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(now - s_runtime.wakeup_armed_tick);
    return elapsed_ms > (uint32_t)CONFIG_VOICE_WAKEUP_WINDOW_MS;
}
#endif

#if CONFIG_ENABLE_LOCAL_OFFLINE_WAKEUP
static bool app_local_wakeup_should_pause(void)
{
    return s_runtime.is_processing ||
           s_runtime.debug_playback_busy ||
           s_runtime.is_recording ||
           s_runtime.voice_round_req;
}

static int app_local_wakeup_peak_abs(const uint8_t *pcm, int pcm_len)
{
    if (pcm == NULL || pcm_len < 2) {
        return 0;
    }

    const int16_t *samples = (const int16_t *)pcm;
    int sample_count = pcm_len / (int)sizeof(int16_t);
    int peak = 0;
    for (int i = 0; i < sample_count; ++i) {
        int v = samples[i];
        if (v == INT16_MIN) {
            v = INT16_MAX;
        } else if (v < 0) {
            v = -v;
        }
        if (v > peak) {
            peak = v;
        }
    }
    return peak;
}

static void app_local_wakeup_task(void *arg)
{
    (void)arg;

    const int sample_rate_hz = 16000;
    const int chunk_ms = CONFIG_LOCAL_WAKEUP_CHUNK_MS;
    const int chunk_bytes = (sample_rate_hz * chunk_ms * PCM_S16LE_BYTES_PER_SAMPLE) / 1000;
    const int threshold = CONFIG_LOCAL_WAKEUP_PEAK_THRESHOLD;
    const int sustain_ms = CONFIG_LOCAL_WAKEUP_SUSTAIN_MS;
    const TickType_t cooldown_ticks = pdMS_TO_TICKS(CONFIG_LOCAL_WAKEUP_COOLDOWN_MS);

    uint8_t *chunk = (uint8_t *)malloc(chunk_bytes);
    if (chunk == NULL) {
        ESP_LOGE(TAG, "Local wakeup: alloc failed (%d bytes)", chunk_bytes);
        vTaskDelete(NULL);
        return;
    }

    bool capture_started = false;
    int active_ms = 0;

    ESP_LOGI(TAG,
             "Local wakeup enabled: chunk=%dms threshold=%d sustain=%dms cooldown=%dms",
             chunk_ms,
             threshold,
             sustain_ms,
             CONFIG_LOCAL_WAKEUP_COOLDOWN_MS);

    while (true) {
        if (app_local_wakeup_should_pause()) {
            if (capture_started) {
                (void)audio_stream_stop_capture();
                capture_started = false;
            }
            active_ms = 0;
            vTaskDelay(pdMS_TO_TICKS(60));
            continue;
        }

        if (!capture_started) {
            esp_err_t err = audio_stream_start_capture();
            if (err != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            capture_started = true;
            active_ms = 0;
        }

        int pcm_len = 0;
        esp_err_t err = audio_stream_read_capture_chunk(chunk, chunk_bytes, &pcm_len);
        if (err != ESP_OK || pcm_len <= 0) {
            active_ms = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int peak = app_local_wakeup_peak_abs(chunk, pcm_len);
        if (peak >= threshold) {
            active_ms += chunk_ms;
        } else {
            if (active_ms > chunk_ms) {
                active_ms -= chunk_ms;
            } else {
                active_ms = 0;
            }
        }

        if (active_ms < sustain_ms) {
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - s_runtime.local_wakeup_last_trigger_tick) < cooldown_ticks) {
            active_ms = 0;
            continue;
        }

        s_runtime.local_wakeup_last_trigger_tick = now;
        active_ms = 0;

        (void)audio_stream_stop_capture();
        capture_started = false;

        if (!network_is_connected() ||
            s_runtime.ai_service == NULL ||
            s_runtime.voice_gateway_client == NULL) {
            ESP_LOGW(TAG, "Local wakeup ignored: runtime not ready");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (!s_runtime.voice_round_req && !s_runtime.is_processing) {
            s_runtime.voice_round_req = true;
            ESP_LOGI(TAG, "Local offline wakeup triggered");
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif

static esp_err_t app_runtime_process_chat_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "User said: %s", text);

    char assistant_text[AI_MAX_RESPONSE_SIZE] = {0};
    esp_err_t err = app_runtime_run_chat_turn(text, assistant_text, sizeof(assistant_text));
    if (err != ESP_OK) {
        return err;
    }

    app_config_t *app_config = config_get();
    (void)audio_set_volume(app_config->volume);
    return ESP_OK;
}

static const char *app_chat_provider_name(ai_provider_config_t provider)
{
    switch (provider) {
        case AI_PROVIDER_CONFIG_OPENAI:
            return "OpenAI";
        case AI_PROVIDER_CONFIG_ZHIPU:
            return "Zhipu AI";
        case AI_PROVIDER_CONFIG_DEEPSEEK:
            return "DeepSeek";
        case AI_PROVIDER_CONFIG_KIMI:
            return "Kimi";
        case AI_PROVIDER_CONFIG_MINIMAX:
            return "MiniMax";
        case AI_PROVIDER_CONFIG_OPENROUTER:
            return "OpenRouter";
        default:
            return "Unknown";
    }
}

static void app_mask_api_key(const char *api_key, char *masked, size_t masked_size)
{
    if (masked == NULL || masked_size == 0) {
        return;
    }

    masked[0] = '\0';

    if (api_key == NULL || api_key[0] == '\0') {
        (void)snprintf(masked, masked_size, "<empty>");
        return;
    }

    size_t key_len = strlen(api_key);
    if (key_len <= 8) {
        (void)snprintf(masked, masked_size, "<set:%u chars>", (unsigned)key_len);
        return;
    }

    int prefix_len = 4;
    int suffix_len = 4;
    const char *suffix = api_key + (key_len - (size_t)suffix_len);

    (void)snprintf(masked,
                   masked_size,
                   "%.*s...%.*s (%u)",
                   prefix_len,
                   api_key,
                   suffix_len,
                   suffix,
                   (unsigned)key_len);
}

static void app_runtime_log_chat_config(const app_config_t *config)
{
    if (config == NULL) {
        return;
    }

    char masked_key[48] = {0};
    app_mask_api_key(config->api_key, masked_key, sizeof(masked_key));

    ESP_LOGI(TAG,
             "Chat cfg: provider=%s model=%s base_url=%s api_key=%s",
             app_chat_provider_name(config->provider),
             (config->model_name[0] != '\0') ? config->model_name : "<empty>",
             (config->base_url[0] != '\0') ? config->base_url : "<empty>",
             masked_key);

    if (config->provider == AI_PROVIDER_CONFIG_OPENROUTER) {
        ESP_LOGI(TAG,
                 "OpenRouter headers: referer=%s x_title=%s",
                 (config->openrouter_http_referer[0] != '\0') ? config->openrouter_http_referer : "<empty>",
                 (config->openrouter_x_title[0] != '\0') ? config->openrouter_x_title : "<empty>");
    }
}

static uint32_t app_runtime_elapsed_ms(TickType_t start_tick, TickType_t end_tick)
{
    if (start_tick == 0 || end_tick == 0) {
        return 0;
    }
    return (uint32_t)pdTICKS_TO_MS(end_tick - start_tick);
}

static void app_stt_debug_trace_init(stt_debug_trace_t *trace,
                                     const char *session_id,
                                     int sample_rate_hz,
                                     int chunk_ms,
                                     int capture_ms)
{
    if (trace == NULL) {
        return;
    }

    memset(trace, 0, sizeof(*trace));
    trace->round_start_tick = xTaskGetTickCount();
    trace->sample_rate_hz = sample_rate_hz;
    trace->requested_chunk_ms = chunk_ms;
    trace->requested_capture_ms = capture_ms;
    trace->fail_chunk_index = -1;

    if (session_id != NULL) {
        strncpy(trace->session_id, session_id, sizeof(trace->session_id) - 1);
        trace->session_id[sizeof(trace->session_id) - 1] = '\0';
    }

    int snapshot_cap = (sample_rate_hz * capture_ms * PCM_S16LE_BYTES_PER_SAMPLE) / 1000;
    if (snapshot_cap <= 0) {
        snapshot_cap = sample_rate_hz * PCM_S16LE_BYTES_PER_SAMPLE;
    }
    if (snapshot_cap > STT_DEBUG_PCM_SNAPSHOT_MAX_BYTES) {
        snapshot_cap = STT_DEBUG_PCM_SNAPSHOT_MAX_BYTES;
    }

    trace->pcm_snapshot = (uint8_t *)malloc(snapshot_cap);
    if (trace->pcm_snapshot != NULL) {
        trace->pcm_snapshot_cap = snapshot_cap;
    } else {
        ESP_LOGW(TAG, "STT debug: snapshot alloc failed (%d bytes)", snapshot_cap);
    }
}

static void app_stt_debug_trace_append_pcm(stt_debug_trace_t *trace,
                                           const uint8_t *pcm,
                                           int len)
{
    if (trace == NULL || pcm == NULL || len <= 0 ||
        trace->pcm_snapshot == NULL || trace->pcm_snapshot_cap <= trace->pcm_snapshot_len) {
        return;
    }

    int remain = trace->pcm_snapshot_cap - trace->pcm_snapshot_len;
    if (remain <= 0) {
        return;
    }

    int copy_len = (len < remain) ? len : remain;
    memcpy(trace->pcm_snapshot + trace->pcm_snapshot_len, pcm, copy_len);
    trace->pcm_snapshot_len += copy_len;
}

#if CONFIG_SDCARD_ENABLED
static esp_err_t app_stt_debug_dump_snapshot(const stt_debug_trace_t *trace)
{
    if (trace == NULL || trace->pcm_snapshot == NULL || trace->pcm_snapshot_len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    typedef struct __attribute__((packed)) {
        char riff[4];
        uint32_t chunk_size;
        char wave[4];
        char fmt[4];
        uint32_t subchunk1_size;
        uint16_t audio_format;
        uint16_t num_channels;
        uint32_t sample_rate;
        uint32_t byte_rate;
        uint16_t block_align;
        uint16_t bits_per_sample;
        char data[4];
        uint32_t data_size;
    } wav_header_t;

    esp_err_t err = storage_sdcard_mount();
    if (err != ESP_OK) {
        return err;
    }

    char file_path[160] = {0};
    int written = snprintf(file_path,
                           sizeof(file_path),
                           "%s/stt_fail_%08lx.wav",
                           storage_sdcard_get_mount_point(),
                           (unsigned long)xTaskGetTickCount());
    if (written <= 0 || (size_t)written >= sizeof(file_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *fp = fopen(file_path, "wb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "STT debug: open failed for %s", file_path);
        return ESP_FAIL;
    }

    wav_header_t header = {0};
    memcpy(header.riff, "RIFF", 4);
    memcpy(header.wave, "WAVE", 4);
    memcpy(header.fmt, "fmt ", 4);
    memcpy(header.data, "data", 4);
    header.subchunk1_size = 16;
    header.audio_format = 1;
    header.num_channels = 1;
    header.sample_rate = (trace->sample_rate_hz > 0) ? (uint32_t)trace->sample_rate_hz : 16000;
    header.bits_per_sample = 16;
    header.block_align = header.num_channels * (header.bits_per_sample / 8);
    header.byte_rate = header.sample_rate * header.block_align;
    header.data_size = (uint32_t)trace->pcm_snapshot_len;
    header.chunk_size = 36 + header.data_size;

    size_t header_written = fwrite(&header, 1, sizeof(header), fp);
    size_t data_written = fwrite(trace->pcm_snapshot, 1, trace->pcm_snapshot_len, fp);
    fclose(fp);

    if (header_written != sizeof(header) || data_written != (size_t)trace->pcm_snapshot_len) {
        ESP_LOGW(TAG, "STT debug: write failed for %s", file_path);
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "STT debug: fail snapshot saved: %s (pcm=%d bytes)",
             file_path,
             trace->pcm_snapshot_len);
    return ESP_OK;
}
#endif

static void app_stt_debug_trace_log(const stt_debug_trace_t *trace,
                                    const char *stage,
                                    esp_err_t err,
                                    bool success,
                                    const char *stt_text)
{
    if (trace == NULL) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    uint32_t total_ms = app_runtime_elapsed_ms(trace->round_start_tick, now);
    uint32_t stt_start_ms = app_runtime_elapsed_ms(trace->round_start_tick, trace->stt_start_tick);
    uint32_t capture_ms = app_runtime_elapsed_ms(trace->capture_start_tick, trace->capture_end_tick);
    uint32_t stt_stop_ms = app_runtime_elapsed_ms(trace->stt_stop_start_tick, trace->stt_stop_end_tick);
    int text_len = (stt_text != NULL) ? (int)strlen(stt_text) : 0;

    if (success) {
        ESP_LOGI(TAG,
                 "STT trace ok sid=%s sr=%d chunk=%dms planned=%d sent=%d empty=%d bytes=%d "
                 "t_start=%ums capture=%ums stop=%ums total=%ums text_len=%d",
                 trace->session_id,
                 trace->sample_rate_hz,
                 trace->effective_chunk_ms,
                 trace->planned_chunks,
                 trace->sent_chunks,
                 trace->empty_chunks,
                 trace->total_pcm_bytes,
                 stt_start_ms,
                 capture_ms,
                 stt_stop_ms,
                 total_ms,
                 text_len);
    } else {
        ESP_LOGW(TAG,
                 "STT trace fail sid=%s stage=%s err=%s sr=%d chunk=%dms planned=%d sent=%d empty=%d bytes=%d "
                 "fail_chunk=%d t_start=%ums capture=%ums stop=%ums total=%ums",
                 trace->session_id,
                 (stage != NULL) ? stage : "unknown",
                 esp_err_to_name(err),
                 trace->sample_rate_hz,
                 trace->effective_chunk_ms,
                 trace->planned_chunks,
                 trace->sent_chunks,
                 trace->empty_chunks,
                 trace->total_pcm_bytes,
                 trace->fail_chunk_index,
                 stt_start_ms,
                 capture_ms,
                 stt_stop_ms,
                 total_ms);
    }
}

static void app_stt_debug_trace_deinit(stt_debug_trace_t *trace)
{
    if (trace == NULL) {
        return;
    }

    free(trace->pcm_snapshot);
    trace->pcm_snapshot = NULL;
    trace->pcm_snapshot_len = 0;
    trace->pcm_snapshot_cap = 0;
}

static void app_voice_state_changed(voice_state_t from,
                                    voice_state_t to,
                                    const char *reason,
                                    void *user_data)
{
    (void)user_data;
    if (reason != NULL && reason[0] != '\0') {
        ESP_LOGI(TAG, "Voice state changed: %s -> %s (%s)",
                 voice_state_to_string(from),
                 voice_state_to_string(to),
                 reason);
    } else {
        ESP_LOGI(TAG, "Voice state changed: %s -> %s",
                 voice_state_to_string(from),
                 voice_state_to_string(to));
    }
}

static void app_make_voice_turn_id(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    snprintf(out,
             out_size,
             "%s_%08lx",
             s_runtime.conversation.session_id,
             (unsigned long)xTaskGetTickCount());
}

static esp_err_t app_stream_capture_to_gateway_stt(const char *session_id,
                                                   int sample_rate_hz,
                                                   int chunk_ms,
                                                   int capture_ms,
                                                   bool *out_stt_started,
                                                   stt_debug_trace_t *trace)
{
    if (session_id == NULL || sample_rate_hz <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_runtime.voice_gateway_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_stt_started != NULL) {
        *out_stt_started = false;
    }

    if (chunk_ms < 100) {
        chunk_ms = 100;
    }
    if (chunk_ms > 500) {
        chunk_ms = 500;
    }
    if (capture_ms < chunk_ms) {
        capture_ms = chunk_ms;
    }

    if (trace != NULL) {
        trace->effective_chunk_ms = chunk_ms;
        trace->effective_capture_ms = capture_ms;
    }

    int chunk_bytes = (sample_rate_hz * chunk_ms * PCM_S16LE_BYTES_PER_SAMPLE) / 1000;
    if (chunk_bytes < PCM_S16LE_BYTES_PER_SAMPLE) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (trace != NULL) {
        trace->chunk_bytes = chunk_bytes;
    }

    uint8_t *chunk_buf = (uint8_t *)malloc(chunk_bytes);
    if (chunk_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = voice_gateway_stt_start(s_runtime.voice_gateway_client, session_id, sample_rate_hz);
    if (err != ESP_OK) {
        free(chunk_buf);
        return err;
    }
    if (trace != NULL) {
        trace->stt_started = true;
        trace->stt_start_tick = xTaskGetTickCount();
    }
    if (out_stt_started != NULL) {
        *out_stt_started = true;
    }

    bool capture_started = false;
    err = audio_stream_start_capture();
    if (err != ESP_OK) {
        free(chunk_buf);
        return err;
    }
    capture_started = true;
    if (trace != NULL) {
        trace->capture_started = true;
        trace->capture_start_tick = xTaskGetTickCount();
    }

    int total_chunks = (capture_ms + chunk_ms - 1) / chunk_ms;
    if (trace != NULL) {
        trace->planned_chunks = total_chunks;
    }
    for (int i = 0; i < total_chunks; i++) {
        int pcm_len = 0;
        err = audio_stream_read_capture_chunk(chunk_buf, chunk_bytes, &pcm_len);
        if (trace != NULL) {
            trace->read_chunks++;
        }
        if (err != ESP_OK) {
            if (trace != NULL) {
                trace->fail_chunk_index = i;
            }
            ESP_LOGE(TAG, "Capture chunk failed at %d/%d: %s",
                     i + 1, total_chunks, esp_err_to_name(err));
            break;
        }
        if (pcm_len <= 0) {
            if (trace != NULL) {
                trace->empty_chunks++;
            }
            continue;
        }

        if (trace != NULL) {
            app_stt_debug_trace_append_pcm(trace, chunk_buf, pcm_len);
        }

        err = voice_gateway_stt_send_audio(s_runtime.voice_gateway_client,
                                           session_id,
                                           chunk_buf,
                                           pcm_len);
        if (err != ESP_OK) {
            if (trace != NULL) {
                trace->fail_chunk_index = i;
            }
            ESP_LOGE(TAG, "Upload chunk failed at %d/%d: %s",
                     i + 1, total_chunks, esp_err_to_name(err));
            break;
        }

        if (trace != NULL) {
            trace->sent_chunks++;
            trace->total_pcm_bytes += pcm_len;
        }
    }

    if (trace != NULL) {
        trace->capture_end_tick = xTaskGetTickCount();
    }

    if (capture_started) {
        (void)audio_stream_stop_capture();
    }

    free(chunk_buf);
    return err;
}

static esp_err_t app_runtime_run_chat_turn(const char *user_text,
                                           char *assistant_text,
                                           size_t assistant_text_size)
{
    if (user_text == NULL || user_text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (assistant_text != NULL && assistant_text_size > 0) {
        assistant_text[0] = '\0';
    }
    if (s_runtime.ai_service == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_runtime.is_processing) {
        ESP_LOGW(TAG, "Already processing, ignoring");
        return ESP_ERR_INVALID_STATE;
    }

    s_runtime.is_processing = true;
    if (s_runtime.ui_ready) {
        (void)ui_show_panel(UI_PANEL_LOADING);
    }

    esp_err_t err = conversation_add_message(&s_runtime.conversation, "user", user_text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add user message: %s", esp_err_to_name(err));
        goto done;
    }

    ai_message_t *messages = NULL;
    err = conversation_get_messages(&s_runtime.conversation, &messages);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get message history: %s", esp_err_to_name(err));
        goto done;
    }

    ai_response_t response;
    memset(&response, 0, sizeof(response));

    err = ai_service_chat_with_history(s_runtime.ai_service, messages, &response);
    if (err == ESP_OK && response.is_success && response.content[0] != '\0') {
        ESP_LOGI(TAG, "AI response: %s", response.content);

        (void)conversation_add_message(&s_runtime.conversation, "assistant", response.content);

        if (assistant_text != NULL && assistant_text_size > 0) {
            strncpy(assistant_text, response.content, assistant_text_size - 1);
            assistant_text[assistant_text_size - 1] = '\0';
        }

        if (s_runtime.ui_ready) {
            (void)ui_update_chat_message(user_text, response.content);
            (void)ui_show_panel(UI_PANEL_CHAT);
        }
    } else {
        ESP_LOGE(TAG, "AI request failed: %s", response.error_msg);
        if (s_runtime.ui_ready) {
            (void)ui_update_status("Request failed");
            (void)ui_show_panel(UI_PANEL_MAIN);
        }
        if (err == ESP_OK) {
            err = ESP_FAIL;
        }
    }

done:
    s_runtime.is_processing = false;
    return err;
}

static esp_err_t app_runtime_run_voice_chat_round(void)
{
    if (!network_is_connected()) {
        ESP_LOGW(TAG, "Voice test unavailable: network is disconnected");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_runtime.voice_gateway_client == NULL || s_runtime.ai_service == NULL) {
        ESP_LOGW(TAG, "Voice test unavailable: gateway or AI service is not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_runtime.debug_playback_busy || s_runtime.is_processing) {
        return ESP_ERR_INVALID_STATE;
    }

    app_config_t *cfg = config_get();
    int sample_rate_hz = (cfg->sampling_rate > 0) ? cfg->sampling_rate : 16000;
    int chunk_ms = cfg->audio_chunk_ms;

    char session_id[VOICE_SESSION_ID_MAX] = {0};
    char *stt_text = (char *)calloc(1, VOICE_STT_TEXT_MAX);
    char *assistant_text = (char *)calloc(1, AI_MAX_RESPONSE_SIZE);
    uint8_t *tts_audio = NULL;
    int tts_len = 0;
    bool stt_started = false;
    const char *fail_stage = "capture";
    stt_debug_trace_t stt_trace;

    if (stt_text == NULL || assistant_text == NULL) {
        free(stt_text);
        free(assistant_text);
        ESP_LOGE(TAG, "Voice round buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    app_make_voice_turn_id(session_id, sizeof(session_id));
    app_stt_debug_trace_init(&stt_trace,
                             session_id,
                             sample_rate_hz,
                             chunk_ms,
                             VOICE_CAPTURE_MS);

    if (voice_session_get_state(&s_runtime.voice_session) != VOICE_STATE_IDLE) {
        (void)voice_session_handle_event(&s_runtime.voice_session, VOICE_EVENT_RESET, "new voice round");
    }

    (void)voice_session_handle_event(&s_runtime.voice_session, VOICE_EVENT_START_LISTEN, "capture begin");
    if (s_runtime.ui_ready) {
        (void)ui_update_status("Voice: listening...");
        (void)ui_debug_set_playing_state(false);
        (void)ui_debug_update_status("Voice: listening...");
    }

    esp_err_t err = app_stream_capture_to_gateway_stt(session_id,
                                                      sample_rate_hz,
                                                      chunk_ms,
                                                      VOICE_CAPTURE_MS,
                                                      &stt_started,
                                                      &stt_trace);
    if (err != ESP_OK) {
        goto fail;
    }

    (void)voice_session_handle_event(&s_runtime.voice_session, VOICE_EVENT_SPEECH_END, "capture done");
    if (s_runtime.ui_ready) {
        (void)ui_update_status("Voice: recognizing...");
        (void)ui_debug_update_status("Voice: recognizing...");
    }

    fail_stage = "stt_stop";
    stt_trace.stt_stop_start_tick = xTaskGetTickCount();
    err = voice_gateway_stt_stop(s_runtime.voice_gateway_client, session_id, stt_text, VOICE_STT_TEXT_MAX);
    stt_trace.stt_stop_end_tick = xTaskGetTickCount();
    stt_started = false;
    if (err != ESP_OK) {
        goto fail;
    }
    if (stt_text[0] == '\0') {
        fail_stage = "stt_empty";
        err = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    ESP_LOGI(TAG, "Voice STT text: %s", stt_text);
    (void)voice_session_handle_event(&s_runtime.voice_session, VOICE_EVENT_STT_FINAL, stt_text);

    if (s_runtime.ui_ready) {
        (void)ui_update_status("Voice: thinking...");
        (void)ui_debug_update_status("Voice: thinking...");
    }

    fail_stage = "chat";
    err = app_runtime_run_chat_turn(stt_text, assistant_text, AI_MAX_RESPONSE_SIZE);
    if (err != ESP_OK || assistant_text[0] == '\0') {
        if (err == ESP_OK) {
            err = ESP_FAIL;
        }
        goto fail;
    }

    (void)voice_session_handle_event(&s_runtime.voice_session, VOICE_EVENT_LLM_READY, "chat ready");

    if (s_runtime.ui_ready) {
        (void)ui_update_status("Voice: synthesizing...");
        (void)ui_debug_update_status("Voice: synthesizing...");
    }

    fail_stage = "tts";
    err = voice_gateway_tts_synthesize(s_runtime.voice_gateway_client,
                                       session_id,
                                       assistant_text,
                                       cfg->tts_voice_name,
                                       &tts_audio,
                                       &tts_len);
    if (err != ESP_OK || tts_audio == NULL || tts_len <= 0) {
        if (err == ESP_OK) {
            err = ESP_FAIL;
        }
        goto fail;
    }

    (void)voice_session_handle_event(&s_runtime.voice_session, VOICE_EVENT_TTS_START, "tts queued");

    (void)audio_set_volume(cfg->volume);
    fail_stage = "playback";
    err = audio_stream_play_chunk(tts_audio, tts_len);
    if (err != ESP_OK) {
        goto fail;
    }

    s_runtime.debug_playback_busy = true;
    if (s_runtime.ui_ready) {
        (void)ui_update_status("Voice: speaking...");
        (void)ui_debug_set_playing_state(true);
        (void)ui_debug_update_status("Voice: speaking...");
    }

    app_stt_debug_trace_log(&stt_trace, "ok", ESP_OK, true, stt_text);
    app_stt_debug_trace_deinit(&stt_trace);
    free(tts_audio);
    free(stt_text);
    free(assistant_text);
    return ESP_OK;

fail:
    app_stt_debug_trace_log(&stt_trace, fail_stage, err, false, stt_text);
#if CONFIG_SDCARD_ENABLED
    if ((strcmp(fail_stage, "capture") == 0 || strncmp(fail_stage, "stt", 3) == 0) &&
        stt_trace.pcm_snapshot_len > 0) {
        esp_err_t dump_err = app_stt_debug_dump_snapshot(&stt_trace);
        if (dump_err != ESP_OK) {
            ESP_LOGW(TAG, "STT debug: dump skipped (%s)", esp_err_to_name(dump_err));
        }
    }
#endif

    if (tts_audio != NULL) {
        free(tts_audio);
    }
    free(stt_text);
    free(assistant_text);
    (void)audio_stream_stop_capture();
    if (stt_started) {
        char ignored_text[8] = {0};
        (void)voice_gateway_stt_stop(s_runtime.voice_gateway_client,
                                     session_id,
                                     ignored_text,
                                     sizeof(ignored_text));
    }
    (void)voice_session_handle_event(&s_runtime.voice_session, VOICE_EVENT_ERROR, "voice round failed");
    (void)voice_session_handle_event(&s_runtime.voice_session, VOICE_EVENT_RESET, "voice round reset");
    app_stt_debug_trace_deinit(&stt_trace);
    return err;
}

static void app_handle_voice_round_request(void)
{
    s_runtime.is_debug_mode = false;

    if (!network_is_connected()) {
        ESP_LOGW(TAG, "Voice round skipped: network is disconnected");
        if (s_runtime.ui_ready) {
            (void)ui_update_status("Network disconnected");
        }
        return;
    }
    if (s_runtime.ai_service == NULL) {
        ESP_LOGW(TAG, "Voice round skipped: AI service is not initialized (check chat provider API key/base URL/model)");
        if (s_runtime.ui_ready) {
            (void)ui_update_status("AI not ready");
        }
        return;
    }

    /* No STT/TTS gateway yet: still exercise DeepSeek via a short text turn. */
    if (s_runtime.voice_gateway_client == NULL) {
        ESP_LOGW(TAG, "Voice gateway unavailable, falling back to text chat");
        if (s_runtime.ui_ready) {
            (void)ui_update_status("Thinking...");
        }

        esp_err_t err = app_runtime_process_chat_text(
            "Say hi to your electronic pet owner in one short Chinese sentence.");
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Text chat fallback failed: %s", esp_err_to_name(err));
            if (s_runtime.ui_ready) {
                char status[96] = {0};
                snprintf(status, sizeof(status), "Chat failed: %s", esp_err_to_name(err));
                (void)ui_update_status(status);
            }
            return;
        }

        if (s_runtime.ui_ready) {
            (void)ui_update_status("Ready");
        }
        return;
    }

    esp_err_t err = app_runtime_run_voice_chat_round();
    if (err == ESP_OK) {
        return;
    }

    ESP_LOGW(TAG, "Voice round failed: %s", esp_err_to_name(err));
    if (s_runtime.ui_ready) {
        char status[96] = {0};
        snprintf(status, sizeof(status), "Voice failed: %s", esp_err_to_name(err));
        (void)ui_update_status(status);
        (void)ui_debug_set_playing_state(false);
        (void)ui_debug_update_status(status);
    }
}

static void app_chat_worker_task(void *arg)
{
    (void)arg;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        app_handle_voice_round_request();
    }
}

static void app_handle_debug_record_request(void)
{
    s_runtime.is_debug_mode = true;

    if (!s_runtime.is_recording) {
        if (audio_debug_start_monitor() == ESP_OK) {
            s_runtime.is_recording = true;
            if (s_runtime.ui_ready) {
                (void)ui_debug_set_recording_state(true);
                (void)ui_debug_update_status("Mic monitor ON");
            }
        }
    } else {
        (void)audio_debug_stop_monitor();
        s_runtime.is_recording = false;
        if (s_runtime.ui_ready) {
            (void)ui_debug_set_recording_state(false);
            (void)ui_debug_update_status("Mic monitor OFF");
        }
    }
}

static void app_handle_debug_play_record_request(void)
{
    s_runtime.is_debug_mode = true;
    ESP_LOGI(TAG, "Handling debug play-record request");

    if (s_runtime.is_recording) {
        (void)audio_debug_stop_monitor();
        s_runtime.is_recording = false;
        if (s_runtime.ui_ready) {
            (void)ui_debug_set_recording_state(false);
        }
    }

    if (s_runtime.ui_ready) {
        (void)ui_debug_set_playing_state(false);
        (void)ui_debug_update_status("Recording 5s... speak now!");
    }

    if (s_runtime.debug_playback_busy) {
        ESP_LOGW(TAG, "Debug play-record ignored: playback still running");
        if (s_runtime.ui_ready) {
            (void)ui_debug_update_status("Playback running, wait...");
        }
        return;
    }

    free(s_runtime.recorded_audio);
    s_runtime.recorded_audio = NULL;
    s_runtime.recorded_len = 0;

    if (audio_debug_record_sample(&s_runtime.recorded_audio, &s_runtime.recorded_len) != ESP_OK) {
        ESP_LOGW(TAG, "Debug play: record sample failed");
        if (s_runtime.ui_ready) {
            (void)ui_debug_set_playing_state(false);
            (void)ui_debug_update_status("Sample capture failed");
        }
        return;
    }

    ESP_LOGI(TAG, "Debug play-record: captured %d bytes", s_runtime.recorded_len);
    if (s_runtime.ui_ready) {
        (void)ui_debug_set_playing_state(false);
        (void)ui_debug_update_status("Sample recorded. Press Play.");
    }
}

static void app_handle_debug_play_request(void)
{
    s_runtime.is_debug_mode = true;
    ESP_LOGI(TAG, "Handling debug playback request");

    if (s_runtime.recorded_audio == NULL || s_runtime.recorded_len <= 0) {
        ESP_LOGW(TAG, "Debug playback: no recorded sample available");
        if (s_runtime.ui_ready) {
            (void)ui_debug_set_playing_state(false);
            (void)ui_debug_update_status("No sample. Press Record first.");
        }
        return;
    }

    if (s_runtime.debug_playback_busy) {
        ESP_LOGW(TAG, "Debug playback ignored: playback still running");
        if (s_runtime.ui_ready) {
            (void)ui_debug_update_status("Playback running, wait...");
        }
        return;
    }

    if (s_runtime.ui_ready) {
        (void)ui_debug_set_playing_state(true);
        (void)ui_debug_update_status("Playing recorded sample...");
    }

    int queued_len = s_runtime.recorded_len;
    esp_err_t play_ret = audio_debug_play_sample_ref(s_runtime.recorded_audio, s_runtime.recorded_len);
    if (play_ret != ESP_OK) {
        ESP_LOGW(TAG, "Debug play: playback queue failed: %s", esp_err_to_name(play_ret));
        s_runtime.debug_playback_busy = false;
        if (s_runtime.ui_ready) {
            (void)ui_debug_set_playing_state(false);
            (void)ui_debug_update_status("Sample playback failed");
        }
    } else {
        s_runtime.debug_playback_busy = true;
        ESP_LOGI(TAG, "Debug play: queued %d bytes", queued_len);
    }
}

#if CONFIG_SDCARD_ENABLED
static void app_handle_debug_sdcard_request(void)
{
    s_runtime.is_debug_mode = true;

    esp_err_t err = storage_sdcard_mount();
    if (err != ESP_OK) {
        if (s_runtime.ui_ready) {
            (void)ui_debug_update_status("SD mount failed");
        }
        return;
    }

    if (s_runtime.ui_ready) {
        char status[96] = {0};
        snprintf(status, sizeof(status), "SD ready: %s", storage_sdcard_get_mount_point());
        (void)ui_debug_update_status(status);
    }
}

static void app_handle_debug_test_request(void)
{
    s_runtime.is_debug_mode = true;

    if (s_runtime.voice_gateway_client != NULL) {
        esp_err_t err = app_runtime_run_voice_chat_round();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Voice debug round failed: %s", esp_err_to_name(err));
            if (s_runtime.ui_ready) {
                char status[96] = {0};
                snprintf(status, sizeof(status), "Voice test failed: %s", esp_err_to_name(err));
                (void)ui_debug_set_playing_state(false);
                (void)ui_debug_update_status(status);
            }
        }
        return;
    }

    esp_err_t err = storage_sdcard_mount();
    if (err != ESP_OK) {
        if (s_runtime.ui_ready) {
            (void)ui_debug_update_status("SD mount failed");
        }
        return;
    }

    char mp3_path[SD_MP3_PATH_MAX_LEN] = {0};
    err = storage_sdcard_find_first_mp3(mp3_path, sizeof(mp3_path));
    if (err != ESP_OK) {
        if (s_runtime.ui_ready) {
            (void)ui_debug_update_status("No MP3 on SD");
        }
        return;
    }

    if (s_runtime.ui_ready) {
        (void)ui_debug_set_playing_state(true);
    }

    err = audio_debug_play_mp3_file(mp3_path);
    if (err != ESP_OK) {
        if (s_runtime.ui_ready) {
            (void)ui_debug_set_playing_state(false);
            (void)ui_debug_update_status("MP3 verify failed");
        }
        return;
    }

    if (s_runtime.ui_ready) {
        const char *file_name = strrchr(mp3_path, '/');
        file_name = (file_name == NULL) ? mp3_path : (file_name + 1);

        char status[96] = {0};
        snprintf(status, sizeof(status), "Stage1 MP3: %.64s", file_name);
        (void)ui_debug_update_status(status);
    }
}
#endif

static const char *app_voice_provider_name(voice_provider_config_t provider)
{
    switch (provider) {
        case VOICE_PROVIDER_CONFIG_VOLCENGINE:
            return "volcengine";
        case VOICE_PROVIDER_CONFIG_ALIYUN:
            return "aliyun";
        case VOICE_PROVIDER_CONFIG_CUSTOM:
            return "custom";
        default:
            return "unknown";
    }
}

static esp_err_t app_runtime_initialize_ai_service(void)
{
    app_config_t *config = config_get();
    const char *provider_name = app_chat_provider_name(config->provider);

    app_runtime_log_chat_config(config);

    if (config->api_key[0] == '\0' || config->base_url[0] == '\0' || config->model_name[0] == '\0') {
        ESP_LOGE(TAG,
                 "Active chat provider config incomplete (api_key/base_url/model_name required)");
        if (s_runtime.ui_ready) {
            (void)ui_update_status("Chat provider config incomplete");
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (s_runtime.ai_service != NULL) {
        ai_service_destroy(s_runtime.ai_service);
    }

    ai_provider_type_t provider = (ai_provider_type_t)config->provider;
    s_runtime.ai_service = ai_service_create(provider);
    if (s_runtime.ai_service == NULL) {
        ESP_LOGE(TAG, "Failed to create AI service");
        return ESP_FAIL;
    }

    esp_err_t err = ai_service_init(s_runtime.ai_service, config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize AI service");
        ai_service_destroy(s_runtime.ai_service);
        s_runtime.ai_service = NULL;
        return err;
    }

    if (s_runtime.ui_ready) {
        (void)ui_update_provider(provider_name);
    }
    ESP_LOGI(TAG, "AI service initialized: %s", provider_name);
    return ESP_OK;
}

static esp_err_t app_runtime_initialize_voice_gateway_client(void)
{
    app_config_t *cfg = config_get();

    if (s_runtime.voice_gateway_client != NULL) {
        voice_gateway_client_destroy(s_runtime.voice_gateway_client);
        s_runtime.voice_gateway_client = NULL;
    }

    if (!cfg->enable_voice_gateway) {
        ESP_LOGI(TAG, "Voice gateway disabled in config");
        return ESP_OK;
    }

    if (cfg->voice_gateway_url[0] == '\0') {
        ESP_LOGW(TAG, "Voice gateway enabled but URL is empty");
        return ESP_ERR_INVALID_STATE;
    }

    voice_gateway_client_cfg_t gw_cfg = {
        .base_url = cfg->voice_gateway_url,
        .access_token = cfg->voice_gateway_token,
        .timeout_ms = cfg->stt_timeout_ms,
        .stt_provider = app_voice_provider_name(cfg->stt_provider),
        .stt_model_name = cfg->stt_model_name,
        .stt_api_key = cfg->stt_api_key,
        .stt_app_id = cfg->stt_app_id,
        .stt_access_token = cfg->stt_access_token,
        .stt_secret_key = cfg->stt_secret_key,
        .stt_base_url = cfg->stt_base_url,
        .stt_language = CONFIG_VOLCENGINE_STT_LANGUAGE,
        .stt_enable_itn = CONFIG_VOLCENGINE_STT_ENABLE_ITN,
        .stt_enable_punc = CONFIG_VOLCENGINE_STT_ENABLE_PUNC,
        .stt_enable_nonstream = CONFIG_VOLCENGINE_STT_ENABLE_NONSTREAM,
        .stt_result_type = CONFIG_VOLCENGINE_STT_RESULT_TYPE,
        .stt_end_window_size_ms = CONFIG_VOLCENGINE_STT_END_WINDOW_SIZE_MS,
        .tts_provider = app_voice_provider_name(cfg->tts_provider),
        .tts_model_name = NULL,
        .tts_api_key = cfg->tts_api_key,
        .tts_app_id = cfg->tts_app_id,
        .tts_access_token = cfg->tts_access_token,
        .tts_secret_key = cfg->tts_secret_key,
        .tts_base_url = cfg->tts_base_url,
    };

    s_runtime.voice_gateway_client = voice_gateway_client_create(&gw_cfg);
    if (s_runtime.voice_gateway_client == NULL) {
        ESP_LOGE(TAG, "Failed to create voice gateway client");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Voice gateway client initialised: %s", cfg->voice_gateway_url);
    return ESP_OK;
}

esp_err_t app_runtime_init(bool ui_ready)
{
    s_runtime.ui_ready = ui_ready;

    esp_err_t err = voice_session_init(&s_runtime.voice_session, true);
    if (err != ESP_OK) {
        return err;
    }

    err = voice_session_set_state_callback(&s_runtime.voice_session,
                                           app_voice_state_changed,
                                           NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = conversation_init(&s_runtime.conversation, CONVERSATION_MAX_HISTORY);
    if (err != ESP_OK) {
        return err;
    }

    err = app_runtime_initialize_ai_service();
    if (err != ESP_OK) {
        return err;
    }

    err = app_runtime_initialize_voice_gateway_client();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Voice gateway init skipped: %s", esp_err_to_name(err));
    }

    if (xTaskCreate(app_chat_worker_task,
                    "chat_worker",
                    APP_CHAT_WORKER_STACK_SIZE,
                    NULL,
                    5,
                    &s_runtime.chat_worker_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create chat worker task");
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_ENABLE_LOCAL_OFFLINE_WAKEUP
    s_runtime.local_wakeup_last_trigger_tick = xTaskGetTickCount();
    if (xTaskCreate(app_local_wakeup_task,
                    "local_wakeup",
                    4096,
                    NULL,
                    4,
                    &s_runtime.local_wakeup_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create local wakeup task");
        return ESP_ERR_NO_MEM;
    }
#endif

    return ESP_OK;
}

void app_runtime_process_requests(void)
{
    if (s_runtime.voice_round_req) {
        s_runtime.voice_round_req = false;
        if (s_runtime.chat_worker_task != NULL) {
            (void)xTaskNotifyGive(s_runtime.chat_worker_task);
        } else {
            app_handle_voice_round_request();
        }
    }

    if (s_runtime.debug_record_req) {
        s_runtime.debug_record_req = false;
        app_handle_debug_record_request();
    }
    if (s_runtime.debug_play_record_req) {
        s_runtime.debug_play_record_req = false;
        app_handle_debug_play_record_request();
    }
    if (s_runtime.debug_play_req) {
        s_runtime.debug_play_req = false;
        app_handle_debug_play_request();
    }
#if CONFIG_SDCARD_ENABLED
    if (s_runtime.debug_sdcard_req) {
        s_runtime.debug_sdcard_req = false;
        app_handle_debug_sdcard_request();
    }
    if (s_runtime.debug_test_req) {
        s_runtime.debug_test_req = false;
        app_handle_debug_test_request();
    }
#endif
}

esp_err_t app_runtime_switch_chat_provider(ai_provider_config_t provider)
{
    app_config_t *cfg = config_get();
    if (cfg == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (provider >= AI_PROVIDER_CONFIG_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    ai_provider_config_t old_provider = cfg->provider;
    if (provider == old_provider) {
        ESP_LOGI(TAG, "Chat provider unchanged: %s", app_chat_provider_name(provider));
        return ESP_OK;
    }

    ESP_LOGI(TAG,
             "Switching chat provider: %s -> %s",
             app_chat_provider_name(old_provider),
             app_chat_provider_name(provider));

    esp_err_t err = config_set_ai_provider(provider);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist provider switch: %s", esp_err_to_name(err));
        return err;
    }

    err = app_runtime_initialize_ai_service();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Chat provider switched to %s", app_chat_provider_name(provider));
        return ESP_OK;
    }

    ESP_LOGE(TAG,
             "Provider %s init failed (%s), rolling back",
             app_chat_provider_name(provider),
             esp_err_to_name(err));

    esp_err_t rollback_cfg_err = config_set_ai_provider(old_provider);
    if (rollback_cfg_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Rollback persist failed: %s",
                 esp_err_to_name(rollback_cfg_err));
        return err;
    }

    esp_err_t rollback_init_err = app_runtime_initialize_ai_service();
    if (rollback_init_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Rollback provider init failed: %s",
                 esp_err_to_name(rollback_init_err));
    }

    return err;
}

esp_err_t app_runtime_reload_ai_service(void)
{
    ESP_LOGI(TAG, "Reloading AI service for active provider");
    return app_runtime_initialize_ai_service();
}

esp_err_t app_runtime_test_chat(const char *prompt)
{
    if (s_runtime.ai_service == NULL) {
        ESP_LOGE(TAG, "AI service not ready");
        return ESP_ERR_INVALID_STATE;
    }

    const char *effective_prompt = prompt;
    if (effective_prompt == NULL || effective_prompt[0] == '\0') {
        effective_prompt = "Reply with OK";
    }

    ai_response_t response;
    memset(&response, 0, sizeof(response));

    ESP_LOGI(TAG, "Provider test prompt: %s", effective_prompt);
    esp_err_t err = ai_service_chat(s_runtime.ai_service, effective_prompt, &response);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Provider test failed: %s", esp_err_to_name(err));
        return err;
    }
    if (!response.is_success || response.content[0] == '\0') {
        ESP_LOGE(TAG,
                 "Provider test returned no content: %s",
                 (response.error_msg[0] != '\0') ? response.error_msg : "unknown");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Provider test response: %s", response.content);
    return ESP_OK;
}

void app_runtime_request_voice_round(void)
{
    s_runtime.voice_round_req = true;
}

void app_runtime_request_debug_record(void)
{
    s_runtime.debug_record_req = true;
}

void app_runtime_request_debug_play_record(void)
{
    ESP_LOGI(TAG, "Debug play-record action requested");
    s_runtime.debug_play_record_req = true;
}

void app_runtime_request_debug_play(void)
{
    ESP_LOGI(TAG, "Debug playback action requested");
    s_runtime.debug_play_req = true;
}

void app_runtime_set_debug_play_volume(int volume)
{
    ESP_LOGI(TAG, "Debug playback volume set to %d", volume);
    (void)audio_set_volume(volume);
}

void app_runtime_handle_network_state(net_state_t state, void *user_data)
{
    (void)user_data;

    switch (state) {
        case NET_STATE_DISCONNECTED:
            ESP_LOGI(TAG, "Network disconnected");
            if (s_runtime.ui_ready) {
                (void)ui_update_status("Network disconnected");
            }
            break;
        case NET_STATE_CONNECTING:
            ESP_LOGI(TAG, "Network connecting...");
            if (s_runtime.ui_ready) {
                (void)ui_update_status("Connecting...");
            }
            break;
        case NET_STATE_CONNECTED:
            ESP_LOGI(TAG, "Network connected");
            if (s_runtime.ui_ready) {
                (void)ui_update_status("Connected");
            }
            break;
        case NET_STATE_ERROR:
            ESP_LOGE(TAG, "Network error");
            if (s_runtime.ui_ready) {
                (void)ui_update_status("Connection failed");
            }
            break;
    }
}

void app_runtime_handle_stt_result(const char *text)
{
    if (text == NULL || strlen(text) == 0) {
        ESP_LOGW(TAG, "Empty STT result");
        return;
    }

    const char *input = app_skip_text_spaces(text);
    if (input == NULL || input[0] == '\0') {
        ESP_LOGW(TAG, "Empty STT result after trim");
        return;
    }

#if CONFIG_ENABLE_LOCAL_OFFLINE_WAKEUP
    (void)app_runtime_process_chat_text(input);
    return;
#else
#if CONFIG_ENABLE_VOICE_WAKEUP
    if (s_runtime.wakeup_armed && app_wakeup_window_expired()) {
        s_runtime.wakeup_armed = false;
        ESP_LOGI(TAG, "Wakeup window expired");
    }

    if (!s_runtime.wakeup_armed) {
        const char *query = NULL;
        if (!app_wakeup_match(input, &query)) {
            ESP_LOGI(TAG, "STT ignored (wake phrase required): %s", input);
            if (s_runtime.ui_ready) {
                (void)ui_update_status("Say wake phrase first");
            }
            return;
        }

        if (query == NULL || query[0] == '\0') {
            s_runtime.wakeup_armed = true;
            s_runtime.wakeup_armed_tick = xTaskGetTickCount();
            ESP_LOGI(TAG, "Wake phrase matched, waiting for next utterance");
            if (s_runtime.ui_ready) {
                (void)ui_update_status("Wakeup matched, speak now");
            }
            return;
        }

        s_runtime.wakeup_armed = false;
        (void)app_runtime_process_chat_text(query);
        return;
    }

    s_runtime.wakeup_armed = false;
    (void)app_runtime_process_chat_text(input);
    return;
#else
    (void)app_runtime_process_chat_text(input);
#endif
#endif
}

void app_runtime_handle_audio_playback_complete(void)
{
    ESP_LOGI(TAG, "Audio playback completed");

    if (voice_session_get_state(&s_runtime.voice_session) == VOICE_STATE_SPEAKING) {
        (void)voice_session_handle_event(&s_runtime.voice_session,
                                         VOICE_EVENT_TTS_END,
                                         "audio playback completed");
    }

    if (s_runtime.debug_playback_busy) {
        s_runtime.debug_playback_busy = false;
        if (s_runtime.ui_ready) {
            (void)ui_debug_set_playing_state(false);
        }
    }

    if (s_runtime.is_debug_mode && s_runtime.ui_ready) {
        (void)ui_debug_update_status("Playback completed");
    }
}

void app_runtime_handle_mic_level(int level)
{
    if (s_runtime.is_debug_mode && s_runtime.ui_ready) {
        (void)ui_debug_update_mic_level(level);
    }
}

#if CONFIG_SDCARD_ENABLED
void app_runtime_request_debug_sdcard(void)
{
    s_runtime.debug_sdcard_req = true;
}

void app_runtime_request_debug_test_audio(void)
{
    s_runtime.debug_test_req = true;
}
#endif