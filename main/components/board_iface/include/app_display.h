#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_display_init(void);
void *app_display_get_shared_i2c_bus(void);

/* Screen brightness for the idle-dim power-saving feature. No-op (returns
 * ESP_OK) on boards whose panel has no backlight PWM control. */
esp_err_t app_display_set_brightness(int percent);

/* Enable/disable the speaker power amplifier, for boards where the PA enable
 * line is not a raw GPIO handled directly by the audio component (e.g. behind
 * an I2C IO-expander). No-op on boards that don't need it. */
esp_err_t app_display_enable_speaker_amp(bool enable);

#ifdef __cplusplus
}
#endif
