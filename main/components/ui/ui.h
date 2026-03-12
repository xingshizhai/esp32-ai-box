/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "lvgl.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_PANEL_MAIN,
    UI_PANEL_CHAT,
    UI_PANEL_SETTINGS,
    UI_PANEL_LOADING,
    UI_PANEL_DEBUG
} ui_panel_t;

esp_err_t ui_init(void);
esp_err_t ui_show_panel(ui_panel_t panel);
esp_err_t ui_update_chat_message(const char *user_msg, const char *ai_msg);
esp_err_t ui_update_status(const char *status);
esp_err_t ui_update_provider(const char *provider_name);
void ui_task(void);

esp_err_t ui_debug_update_mic_level(int level);
esp_err_t ui_debug_update_status(const char *status);
esp_err_t ui_debug_set_recording_state(bool recording);
esp_err_t ui_debug_set_playing_state(bool playing);

#ifdef __cplusplus
}
#endif
