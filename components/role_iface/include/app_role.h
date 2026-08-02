#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *id;
    const char *display_name;
    const char *title;
    const char *system_prompt;
    const char *wake_phrase;
    const char *wake_model_filter;
    const char *idle_status;
    const char *main_action_label;
    bool main_action_initiates_speech;
    const char *initiative_prompt;
    /* Optional peer-device handshake before the first initiative. */
    const char *peer_wake_phrase;
    const char *peer_ready_reply;
    bool peer_auto_continue;
    int peer_auto_turn_limit;
} app_role_profile_t;

/* Role selection is deliberately independent of the board BSP. */
const app_role_profile_t *app_role_get(void);

#ifdef __cplusplus
}
#endif
