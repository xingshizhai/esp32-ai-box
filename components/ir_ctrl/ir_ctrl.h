#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Brings up the RMT TX/RX channels from the board's IR GPIO config, loads
 * any previously learned codes from NVS, and registers the ir_learn_code /
 * ir_send_code / ir_list_codes / ir_forget_code AI tools with device_tools.
 *
 * Returns ESP_ERR_INVALID_STATE if the board has no IR GPIOs configured
 * (both -1). Safe to call once during startup; harmless to skip on boards
 * without IR hardware -- callers should log and continue rather than treat
 * failure as fatal. */
esp_err_t ir_ctrl_init(void);

/* Direct (non-AI-tool) entry points, used by both the AI tool handlers in
 * ir_ctrl.c and the on-device IR debug screen (components/ui/ui_debug.c via
 * app_core). All block the caller -- ir_ctrl_learn() up to timeout_ms
 * waiting for a signal, ir_ctrl_send() up to 1s for the transmit to finish
 * -- so call them from a task that can afford to stall, not from an LVGL
 * event callback (see app_handle_debug_ir_learn_request() in app_runtime.c
 * for the accepted "poll a request flag from the main task" pattern already
 * used by the mic/playback debug buttons). */
esp_err_t ir_ctrl_learn(const char *name, uint32_t timeout_ms, size_t *out_symbol_count);
esp_err_t ir_ctrl_send(const char *name);
esp_err_t ir_ctrl_forget(const char *name);
/* Writes an OpenAI-tool-style JSON array of learned code names into result. */
esp_err_t ir_ctrl_list(char *result, size_t result_size);

#ifdef __cplusplus
}
#endif
