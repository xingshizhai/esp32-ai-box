#include "app_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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
    char stt_text[VOICE_STT_TEXT_MAX] = {0};
    char assistant_text[AI_MAX_RESPONSE_SIZE] = {0};
    uint8_t *tts_audio = NULL;
    int tts_len = 0;
    bool stt_started = false;
    const char *fail_stage = "capture";
    stt_debug_trace_t stt_trace;

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
    err = voice_gateway_stt_stop(s_runtime.voice_gateway_client, session_id, stt_text, sizeof(stt_text));
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
    err = app_runtime_run_chat_turn(stt_text, assistant_text, sizeof(assistant_text));
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

    const char *provider_name = "Unknown";
    switch (config->provider) {
        case AI_PROVIDER_CONFIG_OPENAI:
            provider_name = "OpenAI";
            break;
        case AI_PROVIDER_CONFIG_ZHIPU:
            provider_name = "Zhipu AI";
            break;
        case AI_PROVIDER_CONFIG_DEEPSEEK:
            provider_name = "DeepSeek";
            break;
        default:
            break;
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

    return ESP_OK;
}

void app_runtime_process_requests(void)
{
    if (s_runtime.voice_round_req) {
        s_runtime.voice_round_req = false;
        app_handle_voice_round_request();
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

    ESP_LOGI(TAG, "User said: %s", text);

    char assistant_text[AI_MAX_RESPONSE_SIZE] = {0};
    esp_err_t err = app_runtime_run_chat_turn(text, assistant_text, sizeof(assistant_text));
    if (err != ESP_OK) {
        return;
    }

    app_config_t *app_config = config_get();
    (void)audio_set_volume(app_config->volume);
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