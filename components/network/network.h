#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_STATE_DISCONNECTED,
    NET_STATE_CONNECTING,
    NET_STATE_CONNECTED,
    NET_STATE_ERROR
} net_state_t;

typedef void (*net_state_callback_t)(net_state_t state, void *user_data);

esp_err_t network_init(void);
esp_err_t network_start(void);
esp_err_t network_stop(void);
net_state_t network_get_state(void);
esp_err_t network_set_callback(net_state_callback_t callback, void *user_data);
bool network_is_connected(void);

#ifdef __cplusplus
}
#endif
