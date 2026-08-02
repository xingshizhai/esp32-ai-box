#include "app_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"

#include "config.h"
#include "provider_catalog.h"
#include "ai_service.h"
#include "conversation.h"
#include "ui.h"
#include "audio.h"
#include "voice_session.h"
#include "voice_gateway_client.h"
#if CONFIG_ENABLE_LOCAL_OFFLINE_WAKEUP
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_models.h"
#include "model_path.h"
#endif
#if CONFIG_SDCARD_ENABLED
#include "storage.h"
#endif
#include "app_display.h"
#include "app_role.h"

static const char *TAG = "app_runtime";

/* "我在" wake-word ack, 16 kHz mono PCM s16le, embedded via main/CMakeLists.txt EMBED_FILES. */
extern const uint8_t wake_ack_pcm_start[] asm("_binary_wake_ack_pcm_start");
extern const uint8_t wake_ack_pcm_end[] asm("_binary_wake_ack_pcm_end");

/* Screen power saving: dim after CONFIG_ENABLE_LOCAL_OFFLINE_WAKEUP-gated
 * idle time, wake back to full brightness on the next WakeNet trigger. */
#define SCREEN_DIM_TIMEOUT_MS         (30000)
#define SCREEN_DIM_BRIGHTNESS_PERCENT (15)
#define SCREEN_FULL_BRIGHTNESS_PERCENT (100)

/* Two wake modes:
 *   - "未触发" (untriggered): default. Only the "你好小智" WakeNet keyword
 *     starts a round.
 *   - "已唤醒" (awake/conversation): opened for CONVERSATION_FOLLOWUP_MS
 *     after a round finishes speaking. While open, any VAD-detected speech
 *     starts the next round directly (no wake word needed). If nothing is
 *     said before the window elapses, it silently closes and the device
 *     falls back to requiring the wake word again. */
#define CONVERSATION_FOLLOWUP_MS (10000)
/* No AEC on this board: the mic can still pick up the tail/room echo of
 * the device's own TTS playback for a moment right after it "completes".
 * Ignore VAD triggers for this long after the window opens, or that echo
 * gets mistaken for the start of a follow-up utterance (observed on
 * hardware: playback ended, window opened, VAD fired 190ms later on
 * nothing said, STT came back with a single garbage character). */
#define CONVERSATION_ECHO_GUARD_MS (900)
/* The peer may answer almost immediately after our wake phrase. Start ASR
 * quickly; matching the explicit ready reply rejects our own acoustic tail. */

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
    volatile bool initiative_req;
    bool peer_handshake_confirmed;
    volatile bool peer_reply_expected;
    volatile bool peer_silence_followup_req;
    int peer_auto_turns;
    int selected_mode_index;
    int selected_topic_index;
    volatile bool session_stop_requested;
    bool role_session_active;
    TaskHandle_t chat_worker_task;
#if CONFIG_ENABLE_LOCAL_OFFLINE_WAKEUP
    TaskHandle_t local_wakeup_task;
    TickType_t local_wakeup_last_trigger_tick;
#endif
    TickType_t last_activity_tick;
    bool screen_dimmed;
    volatile bool conversation_mode_active;
    TickType_t conversation_deadline_tick;
    TickType_t conversation_earliest_trigger_tick;
    bool wakeup_armed;
    TickType_t wakeup_armed_tick;
#if CONFIG_SDCARD_ENABLED
    volatile bool debug_sdcard_req;
    volatile bool debug_test_req;
#endif
} app_runtime_state_t;

static app_runtime_state_t s_runtime = {0};

/* Fixed-duration capture window (no VAD/silence-based early stop exists yet
 * -- CONFIG_VAD_SILENCE_MS is reserved but unused). 3500ms was tuned for
 * short smart-speaker commands and truncates ordinary sentences; widened to
 * give natural speech room without dragging out short commands too much. */
#define VOICE_CAPTURE_MS             (7000)
#define PEER_HANDSHAKE_CAPTURE_MS    (3500)
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

#ifndef CONFIG_LOCAL_WAKEUP_COOLDOWN_MS
#define CONFIG_LOCAL_WAKEUP_COOLDOWN_MS 2500
#endif

#ifndef CONFIG_PEER_SILENCE_WAIT_MS
#define CONFIG_PEER_SILENCE_WAIT_MS 700
#endif

#ifndef CONFIG_PEER_SILENCE_TIMEOUT_MS
#define CONFIG_PEER_SILENCE_TIMEOUT_MS 15000
#endif

#ifndef CONFIG_PEER_SPEECH_LEVEL_THRESHOLD
#define CONFIG_PEER_SPEECH_LEVEL_THRESHOLD 600
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

#ifndef CONFIG_VOICE_GATEWAY_MODE_EMBEDDED
#define CONFIG_VOICE_GATEWAY_MODE_EMBEDDED 0
#endif
#ifndef CONFIG_VOICE_GATEWAY_MODE_EXTERNAL
#define CONFIG_VOICE_GATEWAY_MODE_EXTERNAL 0
#endif
#ifndef CONFIG_VOICE_DASHSCOPE_API_KEY
#define CONFIG_VOICE_DASHSCOPE_API_KEY ""
#endif
#ifndef CONFIG_VOICE_DASHSCOPE_WEBSOCKET_URL
#define CONFIG_VOICE_DASHSCOPE_WEBSOCKET_URL "wss://dashscope.aliyuncs.com/api-ws/v1/inference/"
#endif
#ifndef CONFIG_VOICE_DASHSCOPE_STT_MODEL
#define CONFIG_VOICE_DASHSCOPE_STT_MODEL "fun-asr-realtime"
#endif
#ifndef CONFIG_VOICE_DASHSCOPE_TTS_MODEL
#define CONFIG_VOICE_DASHSCOPE_TTS_MODEL "cosyvoice-v3-flash"
#endif
#ifndef CONFIG_VOICE_DASHSCOPE_TTS_VOICE
#define CONFIG_VOICE_DASHSCOPE_TTS_VOICE "longanyang"
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
static esp_err_t app_runtime_run_role_initiative(void);
static esp_err_t app_runtime_wait_for_peer_silence(void);
static esp_err_t app_runtime_initialize_ai_service(void);
static esp_err_t app_runtime_initialize_voice_gateway_client(void);
static esp_err_t app_runtime_reset_role_conversation(void);
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

static const char *app_runtime_tts_voice(const app_config_t *cfg)
{
    const app_role_profile_t *role = app_role_get();
    if (role->tts_voice_name != NULL && role->tts_voice_name[0] != '\0') {
        return role->tts_voice_name;
    }
    return cfg != NULL ? cfg->tts_voice_name : NULL;
}

static esp_err_t app_runtime_synthesize(const app_config_t *cfg,
                                        const char *session_id,
                                        const char *text,
                                        uint8_t **audio,
                                        int *audio_len)
{
    const char *preferred_voice = app_runtime_tts_voice(cfg);
    esp_err_t err = voice_gateway_tts_synthesize(s_runtime.voice_gateway_client,
                                                  session_id,
                                                  text,
                                                  preferred_voice,
                                                  audio,
                                                  audio_len);
    const char *fallback_voice = cfg != NULL ? cfg->tts_voice_name : NULL;
    bool can_fallback = err != ESP_OK && fallback_voice != NULL &&
                        fallback_voice[0] != '\0' && preferred_voice != NULL &&
                        strcmp(preferred_voice, fallback_voice) != 0;
    if (!can_fallback) {
        return err;
    }

    ESP_LOGW(TAG, "Role TTS voice '%s' failed; retrying configured voice '%s'",
             preferred_voice, fallback_voice);
    free(*audio);
    *audio = NULL;
    *audio_len = 0;
    return voice_gateway_tts_synthesize(s_runtime.voice_gateway_client,
                                        session_id,
                                        text,
                                        fallback_voice,
                                        audio,
                                        audio_len);
}

static const app_role_mode_t *app_runtime_selected_mode(void)
{
    const app_role_profile_t *role = app_role_get();
    if (role->modes == NULL || s_runtime.selected_mode_index < 0 ||
        s_runtime.selected_mode_index >= role->mode_count) {
        return NULL;
    }
    return &role->modes[s_runtime.selected_mode_index];
}

static esp_err_t app_runtime_reset_role_conversation(void)
{
    const app_role_profile_t *role = app_role_get();
    const app_role_mode_t *mode = app_runtime_selected_mode();
    esp_err_t err = conversation_clear(&s_runtime.conversation);
    if (err != ESP_OK) {
        return err;
    }
    if (mode == NULL || mode->system_prompt == NULL) {
        return conversation_add_message(&s_runtime.conversation, "system", role->system_prompt);
    }
    size_t size = strlen(role->system_prompt) + strlen(mode->system_prompt) + 4;
    char *combined = malloc(size);
    if (combined == NULL) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(combined, size, "%s\n%s", role->system_prompt, mode->system_prompt);
    err = conversation_add_message(&s_runtime.conversation, "system", combined);
    free(combined);
    return err;
}

static void app_runtime_mode_selected(int mode_index)
{
    const app_role_profile_t *role = app_role_get();
    if (mode_index < 0 || mode_index >= role->mode_count) {
        return;
    }
    s_runtime.selected_mode_index = mode_index;
    s_runtime.selected_topic_index = -1;
    s_runtime.session_stop_requested = false;
    s_runtime.role_session_active = false;
    (void)app_runtime_reset_role_conversation();
    if (s_runtime.ui_ready) {
        char status[64];
        snprintf(status, sizeof(status), "已选择：%s", role->modes[mode_index].label);
        (void)ui_update_status(status);
        (void)ui_set_session_active(false, role->main_action_label);
    }
    ESP_LOGI(TAG, "Role mode selected: %s", role->modes[mode_index].id);
}

static void app_runtime_show_mode_menu(void)
{
    const app_role_profile_t *role = app_role_get();
    if (!s_runtime.ui_ready || role->modes == NULL || role->mode_count <= 0) {
        return;
    }
    const char *labels[4] = {0};
    int count = role->mode_count > 4 ? 4 : role->mode_count;
    for (int i = 0; i < count; ++i) {
        labels[i] = role->modes[i].label;
    }
    (void)ui_show_mode_selection("大神 · 选择对话模式",
                                 labels,
                                 count,
                                 app_runtime_mode_selected);
}

static const char *app_runtime_select_topic_prompt(const app_role_mode_t *mode)
{
    if (mode == NULL || mode->topic_prompts == NULL || mode->topic_prompt_count <= 0) {
        return mode != NULL ? mode->initiative_prompt : NULL;
    }
    if (mode->randomize_topics) {
        s_runtime.selected_topic_index = (int)(esp_random() % (uint32_t)mode->topic_prompt_count);
    } else {
        s_runtime.selected_topic_index =
            (s_runtime.selected_topic_index + 1) % mode->topic_prompt_count;
    }
    ESP_LOGI(TAG, "Mode topic selected: mode=%s topic=%d/%d strategy=%s",
             mode->id,
             s_runtime.selected_topic_index + 1,
             mode->topic_prompt_count,
             mode->randomize_topics ? "random" : "sequential");
    return mode->topic_prompts[s_runtime.selected_topic_index];
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

static void app_runtime_mark_activity(void)
{
    s_runtime.last_activity_tick = xTaskGetTickCount();
    if (s_runtime.screen_dimmed) {
        (void)app_display_set_brightness(SCREEN_FULL_BRIGHTNESS_PERCENT);
        s_runtime.screen_dimmed = false;
        ESP_LOGI(TAG, "Screen: restored to full brightness");
    }
}

/* Called when a voice round finishes speaking: opens (or extends) the
 * "already awake" follow-up window so the next turn doesn't need the wake
 * word, as long as it starts within CONVERSATION_FOLLOWUP_MS. */
static void app_runtime_open_conversation_window(void)
{
    TickType_t now = xTaskGetTickCount();
    s_runtime.conversation_mode_active = true;
    s_runtime.conversation_deadline_tick = now + pdMS_TO_TICKS(CONVERSATION_FOLLOWUP_MS);
    s_runtime.conversation_earliest_trigger_tick = now + pdMS_TO_TICKS(CONVERSATION_ECHO_GUARD_MS);
    if (s_runtime.ui_ready) {
        (void)ui_update_status("Listening (no wake word needed)...");
    }
    ESP_LOGI(TAG, "Conversation window open for %dms (no wake word needed)", CONVERSATION_FOLLOWUP_MS);
}

#if CONFIG_ENABLE_LOCAL_OFFLINE_WAKEUP
static void app_runtime_check_screen_dim(void)
{
    if (s_runtime.screen_dimmed || s_runtime.is_processing || s_runtime.voice_round_req) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    uint32_t idle_ms = (uint32_t)pdTICKS_TO_MS(now - s_runtime.last_activity_tick);
    if (idle_ms < SCREEN_DIM_TIMEOUT_MS) {
        return;
    }

    (void)app_display_set_brightness(SCREEN_DIM_BRIGHTNESS_PERCENT);
    s_runtime.screen_dimmed = true;
    ESP_LOGI(TAG, "Screen: dimmed to %d%% after %" PRIu32 "ms idle", SCREEN_DIM_BRIGHTNESS_PERCENT, idle_ms);
}

static bool app_local_wakeup_should_pause(void)
{
    return s_runtime.is_processing ||
           s_runtime.debug_playback_busy ||
           s_runtime.is_recording ||
           s_runtime.voice_round_req;
}

static void app_local_wakeup_task(void *arg)
{
    (void)arg;

    const TickType_t cooldown_ticks = pdMS_TO_TICKS(CONFIG_LOCAL_WAKEUP_COOLDOWN_MS);

    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "WakeNet: model partition is unavailable");
        vTaskDelete(NULL);
        return;
    }

    const app_role_profile_t *role = app_role_get();
    char *model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, role->wake_model_filter);
    if (model_name == NULL) {
        ESP_LOGE(TAG,
                 "WakeNet unavailable for role=%s: phrase=%s model_filter=%s not found; "
                 "screen action remains available",
                 role->id, role->wake_phrase, role->wake_model_filter);
        if (s_runtime.ui_ready) {
            (void)ui_update_status(role->idle_status);
        }
        esp_srmodel_deinit(models);
        vTaskDelete(NULL);
        return;
    }

    afe_config_t *afe_cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_cfg == NULL) {
        ESP_LOGE(TAG, "WakeNet: AFE config allocation failed");
        esp_srmodel_deinit(models);
        vTaskDelete(NULL);
        return;
    }

    /* Wake-word pipeline, kept lean (no AEC/SE/NS/AGC) to hold CPU/RAM use
     * down. VAD is on (WebRTC VAD, no extra model file) so a follow-up
     * utterance can be detected without repeating the wake word while a
     * conversation window is open -- see app_runtime_open_conversation_window(). */
    afe_cfg->aec_init = false;
    afe_cfg->se_init = false;
    afe_cfg->ns_init = false;
    afe_cfg->vad_init = true;
    /* Higher mode = more restrictive about reporting speech (fewer false
     * positives from background noise); VAD_MODE_2 errs toward not
     * accidentally spending a cloud round on room noise during the
     * follow-up window, at the cost of needing reasonably clear speech. */
    afe_cfg->vad_mode = VAD_MODE_2;
    afe_cfg->vad_model_name = NULL;
    afe_cfg->agc_init = false;
    afe_cfg->wakenet_init = true;
    afe_cfg->wakenet_model_name = model_name;
    afe_cfg->wakenet_model_name_2 = NULL;
    afe_cfg->wakenet_mode = DET_MODE_90;
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    const esp_afe_sr_iface_t *afe_handle = esp_afe_handle_from_config(afe_cfg);
    esp_afe_sr_data_t *afe_data =
        (afe_handle != NULL) ? afe_handle->create_from_config(afe_cfg) : NULL;
    afe_config_free(afe_cfg);
    if (afe_handle == NULL || afe_data == NULL) {
        ESP_LOGE(TAG, "WakeNet: failed to create AFE");
        esp_srmodel_deinit(models);
        vTaskDelete(NULL);
        return;
    }

    const int feed_samples = afe_handle->get_feed_chunksize(afe_data);
    const int chunk_bytes = feed_samples * PCM_S16LE_BYTES_PER_SAMPLE;
    uint8_t *chunk = (uint8_t *)malloc((size_t)chunk_bytes);
    if (chunk == NULL) {
        ESP_LOGE(TAG, "WakeNet: audio buffer allocation failed (%d bytes)", chunk_bytes);
        afe_handle->destroy(afe_data);
        esp_srmodel_deinit(models);
        vTaskDelete(NULL);
        return;
    }

    bool capture_started = false;
    ESP_LOGI(TAG, "WakeNet ready: role=%s phrase=%s model=%s frame=%d samples cooldown=%dms",
             role->id, role->wake_phrase, model_name, feed_samples,
             CONFIG_LOCAL_WAKEUP_COOLDOWN_MS);
    afe_handle->print_pipeline(afe_data);
    if (s_runtime.ui_ready) {
        (void)ui_update_status(role->idle_status);
    }

    while (true) {
        app_runtime_check_screen_dim();

        if (app_local_wakeup_should_pause()) {
            if (capture_started) {
                (void)audio_stream_stop_capture();
                capture_started = false;
            }
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
        }

        int pcm_len = 0;
        esp_err_t err = audio_stream_read_capture_chunk(chunk, chunk_bytes, &pcm_len);
        if (err != ESP_OK || pcm_len <= 0) {
            if (err == ESP_ERR_INVALID_STATE) {
                capture_started = false;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (pcm_len != chunk_bytes) {
            ESP_LOGW(TAG, "WakeNet: short audio frame %d/%d", pcm_len, chunk_bytes);
            continue;
        }

        if (afe_handle->feed(afe_data, (const int16_t *)chunk) < 0) {
            ESP_LOGW(TAG, "WakeNet: AFE feed failed");
            continue;
        }

        afe_fetch_result_t *result = afe_handle->fetch_with_delay(afe_data, pdMS_TO_TICKS(100));
        if (result == NULL || result->ret_value < 0) {
            continue;
        }

        TickType_t now = xTaskGetTickCount();
        bool wake_triggered = (result->wakeup_state == WAKENET_DETECTED);
        bool followup_triggered = false;

        if (!wake_triggered && s_runtime.conversation_mode_active) {
            if ((int32_t)(now - s_runtime.conversation_deadline_tick) >= 0) {
                s_runtime.conversation_mode_active = false;
                if (s_runtime.ui_ready) {
                    (void)ui_update_status(role->idle_status);
                }
                ESP_LOGI(TAG, "Conversation window closed (no follow-up speech), wake word required again");
            } else if (result->vad_state == VAD_SPEECH &&
                       (int32_t)(now - s_runtime.conversation_earliest_trigger_tick) >= 0) {
                followup_triggered = true;
            }
        }

        if (!wake_triggered && !followup_triggered) {
            continue;
        }

        if (wake_triggered && (now - s_runtime.local_wakeup_last_trigger_tick) < cooldown_ticks) {
            continue;
        }

        if (wake_triggered) {
            s_runtime.local_wakeup_last_trigger_tick = now;
        }
        s_runtime.conversation_mode_active = false;
        app_runtime_mark_activity();

        (void)audio_stream_stop_capture();
        capture_started = false;
        afe_handle->reset_buffer(afe_data);

        if (!network_is_connected() ||
            s_runtime.ai_service == NULL ||
            s_runtime.voice_gateway_client == NULL) {
            ESP_LOGW(TAG, "Local wakeup ignored: runtime not ready");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (!s_runtime.voice_round_req && !s_runtime.is_processing) {
            if (wake_triggered) {
                ESP_LOGI(TAG, "WakeNet detected %s (word=%d model=%d channel=%d)",
                         role->wake_phrase,
                         result->wake_word_index,
                         result->wakenet_model_index,
                         result->trigger_channel_id);
                if (s_runtime.ui_ready) {
                    (void)ui_update_status("Wake word detected");
                }

                /* Play "我在" and wait for it to finish before LISTENING
                 * starts. There is no AEC on this board: starting capture
                 * in parallel with the ack (tried earlier) made the device
                 * hear its own "我在" over the mic and process THAT as the
                 * user's command, ignoring whatever the user actually said
                 * next. The product expects a two-step interaction (wake
                 * phrase alone, wait for "我在", then the command), so it's
                 * fine for capture to start only once the ack has fully
                 * played out. A follow-up turn (below) skips this entirely:
                 * playing "我在" again mid-conversation would be intrusive,
                 * and there's no wake phrase to separate from the command
                 * this time anyway. */
                int wake_ack_len = (int)(wake_ack_pcm_end - wake_ack_pcm_start);
                if (audio_play_tts(wake_ack_pcm_start, wake_ack_len) == ESP_OK) {
                    int wake_ack_ms = (wake_ack_len / 2) * 1000 / 16000;
                    vTaskDelay(pdMS_TO_TICKS(wake_ack_ms + 150));
                }
            } else {
                ESP_LOGI(TAG, "Follow-up speech detected within conversation window, no wake word needed");
                if (s_runtime.ui_ready) {
                    (void)ui_update_status("Listening...");
                }
            }

            s_runtime.voice_round_req = true;
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
             provider_catalog_display_name(config->provider),
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
                                                   stt_debug_trace_t *trace,
                                                   const uint8_t *playback_audio,
                                                   int playback_audio_len)
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
    if (playback_audio != NULL && playback_audio_len > 0) {
        /* Establish cloud ASR first, but do not open the microphone while our
         * wake phrase is playing. On LCD-EV hardware, simultaneous playback
         * either monopolizes the codec or overwhelms the mic with self-echo.
         * Mark busy before queueing to avoid a fast completion callback race. */
        s_runtime.debug_playback_busy = true;
        err = audio_stream_play_chunk(playback_audio, playback_audio_len);
        if (err != ESP_OK) {
            s_runtime.debug_playback_busy = false;
            free(chunk_buf);
            return err;
        }
        TickType_t playback_started = xTaskGetTickCount();
        while (s_runtime.debug_playback_busy &&
               app_runtime_elapsed_ms(playback_started, xTaskGetTickCount()) < 10000) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_runtime.debug_playback_busy) {
            s_runtime.debug_playback_busy = false;
            free(chunk_buf);
            return ESP_ERR_TIMEOUT;
        }
    }

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
            /* Chat text panel shows raw AI text and needs full CJK glyph
             * coverage the device doesn't reliably have right now (tofu
             * boxes -- see ui_load_cjk_font()). Stay on the main pet panel
             * and convey state through its mood/expression instead; the
             * debug panel's chat view (still text-based, opt-in only) is
             * unaffected. */
            (void)ui_update_status("Speaking...");
            (void)ui_show_panel(UI_PANEL_MAIN);
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
    s_runtime.peer_silence_followup_req = false;
    if (s_runtime.session_stop_requested) {
        return ESP_ERR_INVALID_STATE;
    }
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
                                                      &stt_trace,
                                                      NULL,
                                                      0);
    if (err != ESP_OK) {
        goto fail;
    }
    if (s_runtime.session_stop_requested) {
        err = ESP_ERR_INVALID_STATE;
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
        s_runtime.peer_silence_followup_req = true;
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
    if (s_runtime.session_stop_requested) {
        err = ESP_ERR_INVALID_STATE;
        goto fail;
    }

    /* Not firing VOICE_EVENT_LLM_READY here: THINKING->SPEAKING already
     * happens below via VOICE_EVENT_TTS_START once synthesis is actually
     * done. Firing both (as this used to) transitioned to SPEAKING early --
     * while still synthesizing, before any audio existed -- and made the
     * later TTS_START land on an already-SPEAKING state every single
     * round, logged as "illegal transition: state=SPEAKING event=TTS_START".
     * Harmless (the state was already correct) but 100% reproducible noise. */

    if (s_runtime.ui_ready) {
        (void)ui_update_status("Voice: synthesizing...");
        (void)ui_debug_update_status("Voice: synthesizing...");
    }

    fail_stage = "tts";
    err = app_runtime_synthesize(cfg, session_id, assistant_text,
                                 &tts_audio, &tts_len);
    if (err != ESP_OK || tts_audio == NULL || tts_len <= 0) {
        if (err == ESP_OK) {
            err = ESP_FAIL;
        }
        goto fail;
    }

    const app_role_profile_t *role = app_role_get();
    if (role->peer_auto_continue) {
        fail_stage = "peer_silence";
        err = app_runtime_wait_for_peer_silence();
        if (err != ESP_OK) {
            goto fail;
        }
    }

    (void)voice_session_handle_event(&s_runtime.voice_session, VOICE_EVENT_TTS_START, "tts queued");

    (void)audio_set_volume(cfg->volume);
    fail_stage = "playback";
    bool arm_peer_reply = role->peer_auto_continue &&
                          s_runtime.peer_handshake_confirmed &&
                          s_runtime.peer_auto_turns < role->peer_auto_turn_limit;
    s_runtime.peer_reply_expected = arm_peer_reply;
    s_runtime.debug_playback_busy = true;
    err = audio_stream_play_chunk(tts_audio, tts_len);
    if (err != ESP_OK) {
        s_runtime.peer_reply_expected = false;
        s_runtime.debug_playback_busy = false;
        goto fail;
    }
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

static esp_err_t app_runtime_capture_peer_reply(char *reply,
                                                size_t reply_size,
                                                const uint8_t *wake_audio,
                                                int wake_audio_len)
{
    if (reply == NULL || reply_size == 0 || s_runtime.voice_gateway_client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_t *cfg = config_get();
    int sample_rate_hz = (cfg->sampling_rate > 0) ? cfg->sampling_rate : 16000;
    int chunk_ms = cfg->audio_chunk_ms;
    char session_id[VOICE_SESSION_ID_MAX] = {0};
    bool stt_started = false;
    stt_debug_trace_t trace;

    app_make_voice_turn_id(session_id, sizeof(session_id));
    app_stt_debug_trace_init(&trace, session_id, sample_rate_hz, chunk_ms,
                             PEER_HANDSHAKE_CAPTURE_MS);

    if (voice_session_get_state(&s_runtime.voice_session) != VOICE_STATE_IDLE) {
        (void)voice_session_handle_event(&s_runtime.voice_session,
                                         VOICE_EVENT_RESET,
                                         "peer handshake listen");
    }
    (void)voice_session_handle_event(&s_runtime.voice_session,
                                     VOICE_EVENT_START_LISTEN,
                                     "waiting for peer ready reply");
    if (s_runtime.ui_ready) {
        (void)ui_update_status("等待小智回答“我在”...");
    }

    esp_err_t err = app_stream_capture_to_gateway_stt(session_id,
                                                      sample_rate_hz,
                                                      chunk_ms,
                                                      PEER_HANDSHAKE_CAPTURE_MS,
                                                      &stt_started,
                                                      &trace,
                                                      wake_audio,
                                                      wake_audio_len);
    if (err != ESP_OK) {
        goto done;
    }

    (void)voice_session_handle_event(&s_runtime.voice_session,
                                     VOICE_EVENT_SPEECH_END,
                                     "peer reply captured");
    trace.stt_stop_start_tick = xTaskGetTickCount();
    err = voice_gateway_stt_stop(s_runtime.voice_gateway_client,
                                 session_id,
                                 reply,
                                 (int)reply_size);
    trace.stt_stop_end_tick = xTaskGetTickCount();
    stt_started = false;
    if (err == ESP_OK && reply[0] == '\0') {
        err = ESP_ERR_NOT_FOUND;
    }

done:
    if (stt_started) {
        char ignored[8] = {0};
        (void)voice_gateway_stt_stop(s_runtime.voice_gateway_client,
                                     session_id,
                                     ignored,
                                     sizeof(ignored));
    }
    if (err != ESP_OK) {
        (void)audio_stream_stop_capture();
    }
    app_stt_debug_trace_log(&trace,
                            err == ESP_OK ? "peer_ack_ok" : "peer_ack_failed",
                            err,
                            err == ESP_OK,
                            reply);
    app_stt_debug_trace_deinit(&trace);
    (void)voice_session_handle_event(&s_runtime.voice_session,
                                     VOICE_EVENT_RESET,
                                     "peer handshake capture complete");
    return err;
}

static esp_err_t app_runtime_wait_for_peer_silence(void)
{
    const int sample_rate_hz = 16000;
    const int frame_ms = 20;
    const int frame_bytes = sample_rate_hz * frame_ms * PCM_S16LE_BYTES_PER_SAMPLE / 1000;
    uint8_t *frame = malloc((size_t)frame_bytes);
    if (frame == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = audio_stream_start_capture();
    if (err != ESP_OK) {
        free(frame);
        return err;
    }

    const TickType_t started = xTaskGetTickCount();
    TickType_t quiet_started = 0;
    int peak_level = 0;
    if (s_runtime.ui_ready) {
        (void)ui_update_status("等待小智说完...");
    }
    ESP_LOGI(TAG,
             "Peer turn gate started: quiet=%dms timeout=%dms threshold=%d",
             CONFIG_PEER_SILENCE_WAIT_MS,
             CONFIG_PEER_SILENCE_TIMEOUT_MS,
             CONFIG_PEER_SPEECH_LEVEL_THRESHOLD);

    while (app_runtime_elapsed_ms(started, xTaskGetTickCount()) <
           CONFIG_PEER_SILENCE_TIMEOUT_MS) {
        if (s_runtime.session_stop_requested) {
            err = ESP_ERR_INVALID_STATE;
            goto done;
        }
        int pcm_len = 0;
        err = audio_stream_read_capture_chunk(frame, frame_bytes, &pcm_len);
        if (err != ESP_OK) {
            break;
        }
        if (pcm_len <= 0) {
            continue;
        }

        const int16_t *samples = (const int16_t *)frame;
        const int sample_count = pcm_len / PCM_S16LE_BYTES_PER_SAMPLE;
        int64_t absolute_sum = 0;
        for (int i = 0; i < sample_count; ++i) {
            int sample = samples[i];
            absolute_sum += sample < 0 ? -sample : sample;
        }
        int level = sample_count > 0 ? (int)(absolute_sum / sample_count) : 0;
        if (level > peak_level) {
            peak_level = level;
        }

        TickType_t now = xTaskGetTickCount();
        if (level > CONFIG_PEER_SPEECH_LEVEL_THRESHOLD) {
            quiet_started = 0;
            continue;
        }
        if (quiet_started == 0) {
            quiet_started = now;
        }
        if (app_runtime_elapsed_ms(quiet_started, now) >= CONFIG_PEER_SILENCE_WAIT_MS) {
            err = ESP_OK;
            ESP_LOGI(TAG, "Peer turn gate opened after quiet interval (peak_level=%d)", peak_level);
            goto done;
        }
    }

    if (err == ESP_OK) {
        err = ESP_ERR_TIMEOUT;
    }
    ESP_LOGW(TAG, "Peer turn gate timed out or failed: %s peak_level=%d",
             esp_err_to_name(err), peak_level);

done:
    (void)audio_stream_stop_capture();
    free(frame);
    return err;
}

static esp_err_t app_runtime_run_role_initiative(void)
{
    const app_role_profile_t *role = app_role_get();
    if (!role->main_action_initiates_speech || role->initiative_prompt == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!network_is_connected() || s_runtime.ai_service == NULL ||
        s_runtime.voice_gateway_client == NULL || s_runtime.debug_playback_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_runtime.session_stop_requested) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Prepare the challenge before waking the peer. Once Xiaozhi says "我在",
     * its command-listening window is already running, so no cloud AI/TTS
     * latency may be inserted between the handshake and our playback. */
    char *assistant_text = calloc(1, AI_MAX_RESPONSE_SIZE);
    uint8_t *tts_audio = NULL;
    int tts_len = 0;
    char session_id[VOICE_SESSION_ID_MAX] = {0};
    app_config_t *cfg = config_get();
    esp_err_t err = ESP_OK;
    if (assistant_text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (s_runtime.ui_ready) {
        (void)ui_update_status("正在准备挑战...");
    }
    const app_role_mode_t *mode = app_runtime_selected_mode();
    bool silence_followup = s_runtime.peer_silence_followup_req;
    s_runtime.peer_silence_followup_req = false;
    const char *initiative_prompt = role->initiative_prompt;
    if (mode != NULL) {
        if (silence_followup && mode->silence_prompt != NULL) {
            initiative_prompt = mode->silence_prompt;
        } else {
            initiative_prompt = app_runtime_select_topic_prompt(mode);
        }
    }
    err = app_runtime_run_chat_turn(initiative_prompt,
                                    assistant_text,
                                    AI_MAX_RESPONSE_SIZE);
    if (err != ESP_OK || assistant_text[0] == '\0') {
        goto done;
    }

    app_make_voice_turn_id(session_id, sizeof(session_id));
    err = app_runtime_synthesize(cfg, session_id, assistant_text,
                                 &tts_audio, &tts_len);
    if (err != ESP_OK || tts_audio == NULL || tts_len <= 0) {
        if (err == ESP_OK) err = ESP_FAIL;
        goto done;
    }

    if (!s_runtime.peer_handshake_confirmed && role->peer_wake_phrase != NULL &&
        role->peer_ready_reply != NULL) {
        char peer_reply[VOICE_STT_TEXT_MAX] = {0};
        uint8_t *wake_audio = NULL;
        int wake_audio_len = 0;
        char wake_session_id[VOICE_SESSION_ID_MAX] = {0};

        if (s_runtime.ui_ready) {
            (void)ui_update_status("正在唤醒小智...");
        }
        app_make_voice_turn_id(wake_session_id, sizeof(wake_session_id));
        esp_err_t wake_err = app_runtime_synthesize(cfg,
                                                    wake_session_id,
                                                    role->peer_wake_phrase,
                                                    &wake_audio,
                                                    &wake_audio_len);
        if (wake_err != ESP_OK || wake_audio == NULL || wake_audio_len <= 0) {
            free(wake_audio);
            err = wake_err == ESP_OK ? ESP_FAIL : wake_err;
            goto done;
        }

        (void)audio_set_volume(cfg->volume);
        ESP_LOGI(TAG,
                 "Peer handshake starting: role=%s phrase=%s expected_reply=%s",
                 role->id,
                 role->peer_wake_phrase,
                 role->peer_ready_reply);
        /* Establish ASR first, finish the wake phrase, then capture. This
         * avoids both WebSocket startup latency and local speaker self-echo. */
        esp_err_t ack_err = app_runtime_capture_peer_reply(peer_reply,
                                                           sizeof(peer_reply),
                                                           wake_audio,
                                                           wake_audio_len);
        free(wake_audio);
        if (ack_err != ESP_OK || strstr(peer_reply, role->peer_ready_reply) == NULL) {
            ESP_LOGW(TAG,
                     "Peer handshake not confirmed: expected=%s recognized=%s err=%s",
                     role->peer_ready_reply,
                     peer_reply[0] != '\0' ? peer_reply : "<empty>",
                     esp_err_to_name(ack_err));
            if (s_runtime.ui_ready) {
                (void)ui_update_status("未听到小智回答，请重试");
            }
            err = ack_err == ESP_OK ? ESP_ERR_NOT_FOUND : ack_err;
            goto done;
        }

        s_runtime.peer_handshake_confirmed = true;
        ESP_LOGI(TAG, "Peer handshake confirmed: reply=%s", peer_reply);
        if (s_runtime.ui_ready) {
            (void)ui_update_status("小智已唤醒，准备挑战...");
        }
    }

    /* Check at the last possible moment: silence opens the turn, speech keeps
     * it closed. The challenge is already buffered, so playback is immediate. */
    if (role->peer_ready_reply != NULL) {
        err = app_runtime_wait_for_peer_silence();
        if (err != ESP_OK) {
            if (s_runtime.ui_ready) {
                (void)ui_update_status("小智仍在说话，本轮已暂停");
            }
            goto done;
        }
    }

    (void)audio_set_volume(cfg->volume);
    bool arm_peer_reply = role->peer_auto_continue &&
                          s_runtime.peer_auto_turns < role->peer_auto_turn_limit;
    s_runtime.peer_reply_expected = arm_peer_reply;
    s_runtime.debug_playback_busy = true;
    err = audio_stream_play_chunk(tts_audio, tts_len);
    if (err == ESP_OK) {
        if (s_runtime.ui_ready) {
            (void)ui_update_status("正在挑战小智...");
        }
    ESP_LOGI(TAG, "Role initiative spoken: role=%s text=%s", role->id, assistant_text);
    } else {
        s_runtime.peer_reply_expected = false;
        s_runtime.debug_playback_busy = false;
    }

done:
    free(tts_audio);
    free(assistant_text);
    return err;
}

static void app_handle_voice_round_request(void)
{
    s_runtime.is_debug_mode = false;
    if (s_runtime.session_stop_requested) {
        return;
    }

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
    const app_role_profile_t *role = app_role_get();
    bool resume_after_silence = s_runtime.peer_silence_followup_req &&
                                role->peer_auto_continue &&
                                s_runtime.role_session_active &&
                                !s_runtime.session_stop_requested &&
                                s_runtime.peer_auto_turns < role->peer_auto_turn_limit;
    if (resume_after_silence) {
        ESP_LOGI(TAG, "Peer silent beyond capture threshold; scheduling proactive mode follow-up");
        if (s_runtime.ui_ready) {
            (void)ui_update_status("小智暂未回答，大神继续话题...");
        }
        s_runtime.initiative_req = true;
        if (s_runtime.chat_worker_task != NULL) {
            (void)xTaskNotifyGive(s_runtime.chat_worker_task);
        }
        return;
    }
    if (s_runtime.ui_ready) {
        char status[96] = {0};
        if (role->peer_auto_continue && s_runtime.peer_auto_turns > 0) {
            snprintf(status, sizeof(status), "未识别到小智回复，本轮结束");
        } else {
            snprintf(status, sizeof(status), "Voice failed: %s", esp_err_to_name(err));
        }
        (void)ui_update_status(status);
        (void)ui_debug_set_playing_state(false);
        (void)ui_debug_update_status(status);
        if (role->peer_auto_continue) {
            s_runtime.role_session_active = false;
            (void)ui_set_session_active(false, role->main_action_label);
        }
    }
}

static void app_chat_worker_task(void *arg)
{
    (void)arg;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_runtime.initiative_req) {
            s_runtime.initiative_req = false;
            esp_err_t err = app_runtime_run_role_initiative();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Role initiative failed: %s", esp_err_to_name(err));
                const app_role_profile_t *role = app_role_get();
                bool handshake_failed = role->peer_ready_reply != NULL &&
                                        !s_runtime.peer_handshake_confirmed;
                if (s_runtime.ui_ready && !handshake_failed) {
                    (void)ui_update_status("主动发言失败");
                }
                s_runtime.role_session_active = false;
                if (s_runtime.ui_ready) {
                    (void)ui_set_session_active(false, role->main_action_label);
                }
            }
        } else {
            app_handle_voice_round_request();
        }
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
        ESP_LOGI(TAG, "Debug playback: no sample, running embedded TTS probe");
        if (s_runtime.ui_ready) {
            (void)ui_debug_set_playing_state(true);
            (void)ui_debug_update_status("TTS probe: synthesizing...");
        }
        app_config_t *cfg = config_get();
        uint8_t *tts_audio = NULL;
        int tts_len = 0;
        esp_err_t tts_err = app_runtime_synthesize(cfg, "debug_tts_probe",
                                                    "你好，我是小智。",
                                                    &tts_audio, &tts_len);
        if (tts_err != ESP_OK) {
            ESP_LOGE(TAG, "Debug TTS probe failed: %s", esp_err_to_name(tts_err));
            free(tts_audio);
            if (s_runtime.ui_ready) {
                (void)ui_debug_set_playing_state(false);
                (void)ui_debug_update_status("TTS probe failed; check serial");
            }
            return;
        }
        ESP_LOGI(TAG, "Debug TTS probe received %d PCM bytes", tts_len);
        esp_err_t play_err = audio_debug_play_owned_sample(&tts_audio, &tts_len);
        if (play_err != ESP_OK) {
            ESP_LOGE(TAG, "Debug TTS probe playback failed: %s", esp_err_to_name(play_err));
            free(tts_audio);
            if (s_runtime.ui_ready) {
                (void)ui_debug_set_playing_state(false);
                (void)ui_debug_update_status("TTS audio queue failed");
            }
        } else {
            s_runtime.debug_playback_busy = true;
            if (s_runtime.ui_ready) {
                (void)ui_debug_update_status("Playing TTS probe...");
            }
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
    const char *provider_name = provider_catalog_display_name(config->provider);

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
    const bool embedded = CONFIG_VOICE_GATEWAY_MODE_EMBEDDED;
    /* DashScope uses the same API-key format for its HTTP and WebSocket
     * services. Keep the dedicated override, but reuse the configured Aliyun
     * provider key by default so embedded mode needs only one credential. */
    const char *embedded_api_key = CONFIG_VOICE_DASHSCOPE_API_KEY[0] != '\0'
                                       ? CONFIG_VOICE_DASHSCOPE_API_KEY
                                       : CONFIG_VOICE_ALIYUN_API_KEY;

    if (s_runtime.voice_gateway_client != NULL) {
        voice_gateway_client_destroy(s_runtime.voice_gateway_client);
        s_runtime.voice_gateway_client = NULL;
    }

    if (!cfg->enable_voice_gateway) {
        ESP_LOGI(TAG, "Voice gateway disabled in config");
        return ESP_OK;
    }

    if (embedded && embedded_api_key[0] == '\0') {
        ESP_LOGW(TAG, "Embedded voice gateway requires a DashScope/Aliyun API key");
        if (s_runtime.ui_ready) {
            (void)ui_update_status("Configure DashScope voice key");
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (!embedded && cfg->voice_gateway_url[0] == '\0') {
        ESP_LOGW(TAG, "Voice gateway enabled but URL is empty");
        return ESP_ERR_INVALID_STATE;
    }

    voice_gateway_client_cfg_t gw_cfg = {
        .embedded = embedded,
        .base_url = cfg->voice_gateway_url,
        .access_token = cfg->voice_gateway_token,
        .timeout_ms = (cfg->tts_timeout_ms > cfg->stt_timeout_ms)
                          ? cfg->tts_timeout_ms
                          : cfg->stt_timeout_ms,
        .embedded_api_key = embedded_api_key,
        .embedded_websocket_url = CONFIG_VOICE_DASHSCOPE_WEBSOCKET_URL,
        .embedded_stt_model = CONFIG_VOICE_DASHSCOPE_STT_MODEL,
        .embedded_tts_model = CONFIG_VOICE_DASHSCOPE_TTS_MODEL,
        .embedded_tts_voice = CONFIG_VOICE_DASHSCOPE_TTS_VOICE,
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

    ESP_LOGI(TAG, "Voice gateway client initialised: mode=%s endpoint=%s",
             embedded ? "embedded" : "external",
             embedded ? CONFIG_VOICE_DASHSCOPE_WEBSOCKET_URL : cfg->voice_gateway_url);
    if (embedded) {
        ESP_LOGI(TAG, "Embedded gateway credential source=%s",
                 CONFIG_VOICE_DASHSCOPE_API_KEY[0] != '\0' ? "dedicated DashScope key"
                                                            : "Aliyun provider key");
    }
    return ESP_OK;
}

esp_err_t app_runtime_init(bool ui_ready)
{
    s_runtime.selected_mode_index = -1;
    s_runtime.ui_ready = ui_ready;
    s_runtime.last_activity_tick = xTaskGetTickCount();
    s_runtime.screen_dimmed = false;

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

    const app_role_profile_t *role = app_role_get();
    (void)conversation_add_message(&s_runtime.conversation, "system", role->system_prompt);
    ESP_LOGI(TAG, "Role selected: id=%s name=%s wake=%s model=%s initiative=%s",
             role->id, role->display_name, role->wake_phrase,
             role->wake_model_filter,
             role->main_action_initiates_speech ? "yes" : "no");
    if (s_runtime.ui_ready) {
        (void)ui_set_role_text(role->title, role->main_action_label);
        (void)ui_update_status(role->idle_status);
        if (role->modes != NULL && role->mode_count > 0) {
            (void)ui_enable_mode_menu(app_runtime_request_mode_menu);
            app_runtime_show_mode_menu();
        }
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
        ESP_LOGI(TAG, "Chat provider unchanged: %s", provider_catalog_display_name(provider));
        return ESP_OK;
    }

    ESP_LOGI(TAG,
             "Switching chat provider: %s -> %s",
             provider_catalog_display_name(old_provider),
             provider_catalog_display_name(provider));

    esp_err_t err = config_set_ai_provider(provider);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist provider switch: %s", esp_err_to_name(err));
        return err;
    }

    err = app_runtime_initialize_ai_service();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Chat provider switched to %s", provider_catalog_display_name(provider));
        return ESP_OK;
    }

    ESP_LOGE(TAG,
             "Provider %s init failed (%s), rolling back",
             provider_catalog_display_name(provider),
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
    app_runtime_mark_activity();
    s_runtime.voice_round_req = true;
}

void app_runtime_request_main_action(void)
{
    const app_role_profile_t *role = app_role_get();
    app_runtime_mark_activity();
    if (role->main_action_initiates_speech) {
        if (role->mode_count > 0 && app_runtime_selected_mode() == NULL) {
            if (s_runtime.ui_ready) {
                (void)ui_update_status("请先选择对话模式");
            }
            return;
        }
        if (!s_runtime.initiative_req && !s_runtime.is_processing) {
            s_runtime.peer_auto_turns = 0;
            /* A normal start chooses a fresh mode topic. Silence recovery
             * keeps the current topic through conversation history. */
            s_runtime.peer_reply_expected = false;
            s_runtime.session_stop_requested = false;
            s_runtime.role_session_active = true;
            if (s_runtime.ui_ready) {
                (void)ui_set_session_active(true, role->main_action_label);
            }
            s_runtime.initiative_req = true;
            if (s_runtime.chat_worker_task != NULL) {
                (void)xTaskNotifyGive(s_runtime.chat_worker_task);
            }
        }
        return;
    }
    app_runtime_request_voice_round();
}

void app_runtime_request_end_session(void)
{
    const app_role_profile_t *role = app_role_get();
    s_runtime.session_stop_requested = true;
    s_runtime.role_session_active = false;
    s_runtime.peer_reply_expected = false;
    s_runtime.voice_round_req = false;
    s_runtime.initiative_req = false;
    s_runtime.peer_auto_turns = role->peer_auto_turn_limit;
    (void)audio_stream_stop_capture();
    (void)voice_session_handle_event(&s_runtime.voice_session,
                                     VOICE_EVENT_RESET,
                                     "session stopped from UI");
    (void)app_runtime_reset_role_conversation();
    if (s_runtime.ui_ready) {
        (void)ui_set_session_active(false, role->main_action_label);
        (void)ui_update_status("会话已结束，可重新开始");
    }
    ESP_LOGI(TAG, "Role session stopped by user");
}

void app_runtime_request_mode_menu(void)
{
    const app_role_profile_t *role = app_role_get();
    if (role->modes == NULL || role->mode_count <= 0) {
        return;
    }
    if (s_runtime.role_session_active || s_runtime.is_processing ||
        s_runtime.initiative_req || s_runtime.peer_reply_expected) {
        app_runtime_request_end_session();
    }
    s_runtime.selected_mode_index = -1;
    s_runtime.session_stop_requested = false;
    (void)app_runtime_reset_role_conversation();
    app_runtime_show_mode_menu();
    ESP_LOGI(TAG, "Returned to role mode menu");
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
    s_runtime.last_activity_tick = xTaskGetTickCount();

    if (voice_session_get_state(&s_runtime.voice_session) == VOICE_STATE_SPEAKING) {
        (void)voice_session_handle_event(&s_runtime.voice_session,
                                         VOICE_EVENT_TTS_END,
                                         "audio playback completed");
        /* Nothing used to reset the main-panel status text after a voice
         * round finished speaking, so it stayed on "Voice: speaking..." at
         * idle indefinitely -- misleading on the standby screen. Opening
         * the conversation window here both fixes that (it sets its own
         * status text) and starts the "already awake" follow-up period:
         * the next turn doesn't need the wake word as long as it starts
         * within CONVERSATION_FOLLOWUP_MS. */
        app_runtime_open_conversation_window();
    }

    if (s_runtime.debug_playback_busy) {
        s_runtime.debug_playback_busy = false;
        if (s_runtime.ui_ready) {
            (void)ui_debug_set_playing_state(false);
        }
    }

    const app_role_profile_t *role = app_role_get();
    if (s_runtime.peer_reply_expected && role->peer_auto_continue &&
        !s_runtime.session_stop_requested) {
        s_runtime.peer_reply_expected = false;
        if (s_runtime.peer_auto_turns < role->peer_auto_turn_limit) {
            ++s_runtime.peer_auto_turns;
            ESP_LOGI(TAG,
                     "Peer reply capture scheduled: turn=%d/%d",
                     s_runtime.peer_auto_turns,
                     role->peer_auto_turn_limit);
            if (s_runtime.ui_ready) {
                (void)ui_update_status("等待小智回复...");
            }
            app_runtime_request_voice_round();
        }
    }
    if (role->peer_auto_continue && s_runtime.role_session_active &&
        s_runtime.peer_auto_turns >= role->peer_auto_turn_limit) {
        s_runtime.role_session_active = false;
        if (s_runtime.ui_ready) {
            (void)ui_set_session_active(false, role->main_action_label);
            (void)ui_update_status("会话轮次完成");
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
