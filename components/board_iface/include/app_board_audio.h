#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Board-owned audio wiring. Codec addresses may be 7-bit or the 8-bit form
 * expected by esp_codec_dev; the audio service normalizes either form. */
typedef struct {
    int i2s_mclk_gpio;
    int i2s_bclk_gpio;
    int i2s_ws_gpio;
    int i2s_dout_gpio;
    int i2s_din_gpio;
    int codec_i2c_port;
    int codec_i2c_scl_gpio;
    int codec_i2c_sda_gpio;
    uint8_t es8311_i2c_addr;
    uint8_t es7210_i2c_addr;
    int pa_gpio;
    bool pa_inverted;
} app_board_audio_config_t;

/* Implemented by exactly one selected board component. */
esp_err_t app_board_get_audio_config(app_board_audio_config_t *config);

#ifdef __cplusplus
}
#endif
