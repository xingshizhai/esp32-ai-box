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

typedef void (*ui_debug_action_callback_t)(void);
typedef void (*ui_debug_volume_callback_t)(int volume);
typedef void (*ui_main_action_callback_t)(void);
typedef void (*ui_mode_select_callback_t)(int mode_index);

esp_err_t ui_init(void);
esp_err_t ui_show_panel(ui_panel_t panel);
esp_err_t ui_update_chat_message(const char *user_msg, const char *ai_msg);
esp_err_t ui_update_status(const char *status);
esp_err_t ui_update_provider(const char *provider_name);
esp_err_t ui_set_main_action_callback(ui_main_action_callback_t callback);
esp_err_t ui_set_role_text(const char *title, const char *action_label);
esp_err_t ui_show_mode_selection(const char *title,
                                 const char *const *labels,
                                 int count,
                                 ui_mode_select_callback_t callback);
esp_err_t ui_set_end_session_callback(ui_main_action_callback_t callback);
esp_err_t ui_enable_mode_menu(ui_main_action_callback_t callback);
esp_err_t ui_set_session_active(bool active, const char *start_label);
void ui_task(void);
esp_err_t ui_font_run_self_test(void);
void ui_font_log_stats(void);

esp_err_t ui_debug_update_mic_level(int level);
esp_err_t ui_debug_update_status(const char *status);
esp_err_t ui_debug_set_recording_state(bool recording);
esp_err_t ui_debug_set_playing_state(bool playing);
esp_err_t ui_debug_set_play_volume(int volume);
esp_err_t ui_debug_set_record_action_callback(ui_debug_action_callback_t callback);
esp_err_t ui_debug_set_play_record_action_callback(ui_debug_action_callback_t callback);
esp_err_t ui_debug_set_play_action_callback(ui_debug_action_callback_t callback);
esp_err_t ui_debug_set_play_volume_callback(ui_debug_volume_callback_t callback);
esp_err_t ui_debug_update_ir_status(const char *status);
esp_err_t ui_debug_set_ir_learn_action_callback(ui_debug_action_callback_t callback);
esp_err_t ui_debug_set_ir_send_action_callback(ui_debug_action_callback_t callback);
esp_err_t ui_debug_set_ir_forget_action_callback(ui_debug_action_callback_t callback);
#if CONFIG_SDCARD_ENABLED
esp_err_t ui_debug_set_sdcard_action_callback(ui_debug_action_callback_t callback);
esp_err_t ui_debug_set_test_audio_action_callback(ui_debug_action_callback_t callback);
#endif

#ifdef __cplusplus
}
#endif
