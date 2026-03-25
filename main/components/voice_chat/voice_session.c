#include "voice_session.h"

#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "voice_session";

/* Some editor indexers may not expand all ESP-IDF error macros for new components. */
#ifndef ESP_ERR_INVALID_ARG
#define ESP_ERR_INVALID_ARG ((esp_err_t)0x102)
#endif
#ifndef ESP_ERR_INVALID_STATE
#define ESP_ERR_INVALID_STATE ((esp_err_t)0x103)
#endif
#ifndef ESP_ERR_NOT_SUPPORTED
#define ESP_ERR_NOT_SUPPORTED ((esp_err_t)0x106)
#endif

static bool voice_transition_allowed(voice_state_t state, voice_event_t event)
{
    switch (state) {
        case VOICE_STATE_IDLE:
            return event == VOICE_EVENT_START_LISTEN ||
                   event == VOICE_EVENT_RESET ||
                   event == VOICE_EVENT_ERROR;
        case VOICE_STATE_LISTENING:
            return event == VOICE_EVENT_SPEECH_END ||
                   event == VOICE_EVENT_INTERRUPT ||
                   event == VOICE_EVENT_RESET ||
                   event == VOICE_EVENT_ERROR;
        case VOICE_STATE_RECOGNIZING:
            return event == VOICE_EVENT_STT_FINAL ||
                   event == VOICE_EVENT_INTERRUPT ||
                   event == VOICE_EVENT_RESET ||
                   event == VOICE_EVENT_ERROR;
        case VOICE_STATE_THINKING:
            return event == VOICE_EVENT_LLM_READY ||
                   event == VOICE_EVENT_INTERRUPT ||
                   event == VOICE_EVENT_RESET ||
                   event == VOICE_EVENT_ERROR;
        case VOICE_STATE_SPEAKING:
            return event == VOICE_EVENT_TTS_END ||
                   event == VOICE_EVENT_INTERRUPT ||
                   event == VOICE_EVENT_RESET ||
                   event == VOICE_EVENT_ERROR;
        case VOICE_STATE_INTERRUPTING:
            return event == VOICE_EVENT_START_LISTEN ||
                   event == VOICE_EVENT_RESET ||
                   event == VOICE_EVENT_ERROR;
        case VOICE_STATE_ERROR:
            return event == VOICE_EVENT_RESET;
        default:
            return false;
    }
}

static voice_state_t voice_next_state(voice_state_t state, voice_event_t event, bool enable_barge_in)
{
    (void)enable_barge_in;

    if (event == VOICE_EVENT_ERROR) {
        return VOICE_STATE_ERROR;
    }

    if (event == VOICE_EVENT_RESET) {
        return VOICE_STATE_IDLE;
    }

    switch (state) {
        case VOICE_STATE_IDLE:
            if (event == VOICE_EVENT_START_LISTEN) {
                return VOICE_STATE_LISTENING;
            }
            break;
        case VOICE_STATE_LISTENING:
            if (event == VOICE_EVENT_SPEECH_END) {
                return VOICE_STATE_RECOGNIZING;
            }
            if (event == VOICE_EVENT_INTERRUPT) {
                return VOICE_STATE_INTERRUPTING;
            }
            break;
        case VOICE_STATE_RECOGNIZING:
            if (event == VOICE_EVENT_STT_FINAL) {
                return VOICE_STATE_THINKING;
            }
            if (event == VOICE_EVENT_INTERRUPT) {
                return VOICE_STATE_INTERRUPTING;
            }
            break;
        case VOICE_STATE_THINKING:
            if (event == VOICE_EVENT_LLM_READY || event == VOICE_EVENT_TTS_START) {
                return VOICE_STATE_SPEAKING;
            }
            if (event == VOICE_EVENT_INTERRUPT) {
                return VOICE_STATE_INTERRUPTING;
            }
            break;
        case VOICE_STATE_SPEAKING:
            if (event == VOICE_EVENT_TTS_END) {
                return VOICE_STATE_IDLE;
            }
            if (event == VOICE_EVENT_INTERRUPT) {
                return VOICE_STATE_INTERRUPTING;
            }
            break;
        case VOICE_STATE_INTERRUPTING:
            if (event == VOICE_EVENT_START_LISTEN) {
                return VOICE_STATE_LISTENING;
            }
            break;
        case VOICE_STATE_ERROR:
            break;
        default:
            break;
    }

    return state;
}

esp_err_t voice_session_init(voice_session_t *session, bool enable_barge_in)
{
    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    session->state = VOICE_STATE_IDLE;
    session->enable_barge_in = enable_barge_in;
    session->state_changed_cb = NULL;
    session->user_data = NULL;

    ESP_LOGI(TAG, "voice session initialized, barge-in=%s", enable_barge_in ? "on" : "off");
    return ESP_OK;
}

esp_err_t voice_session_set_state_callback(voice_session_t *session,
                                           voice_state_changed_cb_t cb,
                                           void *user_data)
{
    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    session->state_changed_cb = cb;
    session->user_data = user_data;
    return ESP_OK;
}

esp_err_t voice_session_handle_event(voice_session_t *session,
                                     voice_event_t event,
                                     const char *reason)
{
    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((event == VOICE_EVENT_INTERRUPT) && !session->enable_barge_in &&
        session->state == VOICE_STATE_SPEAKING) {
        ESP_LOGW(TAG, "interrupt ignored: barge-in disabled");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!voice_transition_allowed(session->state, event)) {
        ESP_LOGW(TAG, "illegal transition: state=%s event=%s",
                 voice_state_to_string(session->state), voice_event_to_string(event));
        return ESP_ERR_INVALID_STATE;
    }

    voice_state_t from = session->state;
    voice_state_t to = voice_next_state(from, event, session->enable_barge_in);
    session->state = to;

    ESP_LOGI(TAG, "state transition: %s --(%s)--> %s%s%s",
             voice_state_to_string(from),
             voice_event_to_string(event),
             voice_state_to_string(to),
             (reason != NULL && reason[0] != '\0') ? " reason=" : "",
             (reason != NULL && reason[0] != '\0') ? reason : "");

    if (session->state_changed_cb != NULL) {
        session->state_changed_cb(from, to, reason, session->user_data);
    }

    return ESP_OK;
}

voice_state_t voice_session_get_state(const voice_session_t *session)
{
    if (session == NULL) {
        return VOICE_STATE_ERROR;
    }
    return session->state;
}

const char *voice_state_to_string(voice_state_t state)
{
    switch (state) {
        case VOICE_STATE_IDLE:
            return "IDLE";
        case VOICE_STATE_LISTENING:
            return "LISTENING";
        case VOICE_STATE_RECOGNIZING:
            return "RECOGNIZING";
        case VOICE_STATE_THINKING:
            return "THINKING";
        case VOICE_STATE_SPEAKING:
            return "SPEAKING";
        case VOICE_STATE_INTERRUPTING:
            return "INTERRUPTING";
        case VOICE_STATE_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

const char *voice_event_to_string(voice_event_t event)
{
    switch (event) {
        case VOICE_EVENT_START_LISTEN:
            return "START_LISTEN";
        case VOICE_EVENT_SPEECH_END:
            return "SPEECH_END";
        case VOICE_EVENT_STT_FINAL:
            return "STT_FINAL";
        case VOICE_EVENT_LLM_READY:
            return "LLM_READY";
        case VOICE_EVENT_TTS_START:
            return "TTS_START";
        case VOICE_EVENT_TTS_END:
            return "TTS_END";
        case VOICE_EVENT_INTERRUPT:
            return "INTERRUPT";
        case VOICE_EVENT_RESET:
            return "RESET";
        case VOICE_EVENT_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}
