#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Board-owned infrared transceiver wiring. Both fields are -1 when the
 * board has no IR hardware wired up (e.g. no ESP32-S3-BOX-3-SENSOR dock
 * attached). */
typedef struct {
    int ir_tx_gpio; /* drives the IR LED (through a driver transistor) */
    int ir_rx_gpio; /* reads the IR receiver module output */
} app_board_ir_config_t;

/* Implemented by exactly one selected board component. */
esp_err_t app_board_get_ir_config(app_board_ir_config_t *config);

#ifdef __cplusplus
}
#endif
