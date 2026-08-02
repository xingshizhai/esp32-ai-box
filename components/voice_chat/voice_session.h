#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_STATE_IDLE = 0,
    VOICE_STATE_LISTENING,
    VOICE_STATE_RECOGNIZING,
    VOICE_STATE_THINKING,
    VOICE_STATE_SPEAKING,
    VOICE_STATE_INTERRUPTING,
    VOICE_STATE_ERROR,
} voice_state_t;

typedef enum {
    VOICE_EVENT_START_LISTEN = 0,
    VOICE_EVENT_SPEECH_END,
    VOICE_EVENT_STT_FINAL,
    VOICE_EVENT_LLM_READY,
    VOICE_EVENT_TTS_START,
    VOICE_EVENT_TTS_END,
    VOICE_EVENT_INTERRUPT,
    VOICE_EVENT_RESET,
    VOICE_EVENT_ERROR,
} voice_event_t;

typedef void (*voice_state_changed_cb_t)(voice_state_t from,
                                         voice_state_t to,
                                         const char *reason,
                                         void *user_data);

typedef struct {
    voice_state_t state;
    bool enable_barge_in;
    voice_state_changed_cb_t state_changed_cb;
    void *user_data;
} voice_session_t;

esp_err_t voice_session_init(voice_session_t *session, bool enable_barge_in);
esp_err_t voice_session_set_state_callback(voice_session_t *session,
                                           voice_state_changed_cb_t cb,
                                           void *user_data);
esp_err_t voice_session_handle_event(voice_session_t *session,
                                     voice_event_t event,
                                     const char *reason);
voice_state_t voice_session_get_state(const voice_session_t *session);
const char *voice_state_to_string(voice_state_t state);
const char *voice_event_to_string(voice_event_t event);

#ifdef __cplusplus
}
#endif
