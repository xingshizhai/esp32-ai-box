/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "ui.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include <string.h>

static const char *TAG = "ui";

static lv_obj_t *s_main_panel = NULL;
static lv_obj_t *s_chat_panel = NULL;
static lv_obj_t *s_settings_panel = NULL;
static lv_obj_t *s_loading_panel = NULL;
static lv_obj_t *s_debug_panel = NULL;

static lv_obj_t *s_mic_level_bar = NULL;
static lv_obj_t *s_mic_level_label = NULL;
static lv_obj_t *s_recording_status = NULL;
static lv_obj_t *s_debug_status = NULL;
static lv_obj_t *s_playing_status = NULL;
static bool s_ui_initialized = false;

esp_err_t ui_init(void)
{
    ESP_LOGI(TAG, "Initializing UI");

    esp_err_t ret = ESP_OK;

    if (lv_display_get_default() == NULL) {
        ESP_LOGE(TAG, "No LVGL display registered, skip UI init");
        return ESP_ERR_INVALID_STATE;
    }

    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "Failed to lock LVGL port during init");
        return ESP_FAIL;
    }
    
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_bg_color(&style, lv_color_black());
    lv_style_set_text_color(&style, lv_color_white());
    
    s_main_panel = lv_obj_create(lv_scr_act());
    if (s_main_panel == NULL) {
        ESP_LOGE(TAG, "Failed to create main panel - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_obj_add_style(s_main_panel, &style, 0);
    lv_obj_set_size(s_main_panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(s_main_panel);
    
    lv_obj_t *title = lv_label_create(s_main_panel);
    if (title == NULL) {
        ESP_LOGE(TAG, "Failed to create title label - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_label_set_text(title, "AI Chat Assistant");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    
    lv_obj_t *info = lv_label_create(s_main_panel);
    if (info == NULL) {
        ESP_LOGE(TAG, "Failed to create info label - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_label_set_text(info, "Touch to start");
    lv_obj_align(info, LV_ALIGN_CENTER, 0, 0);
    
    lv_obj_t *debug_btn = lv_btn_create(s_main_panel);
    if (debug_btn == NULL) {
        ESP_LOGE(TAG, "Failed to create debug button - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_obj_set_size(debug_btn, 120, 40);
    lv_obj_align(debug_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    
    lv_obj_t *debug_label = lv_label_create(debug_btn);
    if (debug_label == NULL) {
        ESP_LOGE(TAG, "Failed to create debug label - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_label_set_text(debug_label, "Debug");
    lv_obj_center(debug_label);
    
    s_chat_panel = lv_obj_create(lv_scr_act());
    if (s_chat_panel == NULL) {
        ESP_LOGE(TAG, "Failed to create chat panel - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_obj_add_style(s_chat_panel, &style, 0);
    lv_obj_set_size(s_chat_panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(s_chat_panel);
    lv_obj_add_flag(s_chat_panel, LV_OBJ_FLAG_HIDDEN);
    
    s_settings_panel = lv_obj_create(lv_scr_act());
    if (s_settings_panel == NULL) {
        ESP_LOGE(TAG, "Failed to create settings panel - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_obj_add_style(s_settings_panel, &style, 0);
    lv_obj_set_size(s_settings_panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(s_settings_panel);
    lv_obj_add_flag(s_settings_panel, LV_OBJ_FLAG_HIDDEN);
    
    s_loading_panel = lv_obj_create(lv_scr_act());
    if (s_loading_panel == NULL) {
        ESP_LOGE(TAG, "Failed to create loading panel - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_obj_add_style(s_loading_panel, &style, 0);
    lv_obj_set_size(s_loading_panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(s_loading_panel);
    lv_obj_add_flag(s_loading_panel, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t *loading_label = lv_label_create(s_loading_panel);
    if (loading_label == NULL) {
        ESP_LOGE(TAG, "Failed to create loading label - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_label_set_text(loading_label, "Processing...");
    lv_obj_align(loading_label, LV_ALIGN_CENTER, 0, 0);
    
    s_debug_panel = lv_obj_create(lv_scr_act());
    if (s_debug_panel == NULL) {
        ESP_LOGE(TAG, "Failed to create debug panel - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_obj_add_style(s_debug_panel, &style, 0);
    lv_obj_set_size(s_debug_panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(s_debug_panel);
    lv_obj_add_flag(s_debug_panel, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t *debug_title = lv_label_create(s_debug_panel);
    if (debug_title == NULL) {
        ESP_LOGE(TAG, "Failed to create debug title - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_label_set_text(debug_title, "Debug Panel");
    lv_obj_set_style_text_align(debug_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(debug_title, LV_ALIGN_TOP_MID, 0, 10);
    
    lv_obj_t *mic_section = lv_label_create(s_debug_panel);
    if (mic_section == NULL) {
        ESP_LOGE(TAG, "Failed to create mic section - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_label_set_text(mic_section, "Microphone Test:");
    lv_obj_align(mic_section, LV_ALIGN_TOP_LEFT, 10, 50);
    
    s_mic_level_bar = lv_bar_create(s_debug_panel);
    if (s_mic_level_bar == NULL) {
        ESP_LOGE(TAG, "Failed to create mic level bar - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_obj_set_size(s_mic_level_bar, LV_HOR_RES - 40, 20);
    lv_obj_align(s_mic_level_bar, LV_ALIGN_TOP_MID, 0, 80);
    lv_bar_set_range(s_mic_level_bar, 0, 100);
    lv_bar_set_value(s_mic_level_bar, 0, LV_ANIM_OFF);
    
    s_mic_level_label = lv_label_create(s_debug_panel);
    if (s_mic_level_label == NULL) {
        ESP_LOGE(TAG, "Failed to create mic level label - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_label_set_text_fmt(s_mic_level_label, "Level: %d%%", 0);
    lv_obj_align(s_mic_level_label, LV_ALIGN_TOP_MID, 0, 110);
    
    s_recording_status = lv_label_create(s_debug_panel);
    if (s_recording_status == NULL) {
        ESP_LOGE(TAG, "Failed to create recording status - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_label_set_text(s_recording_status, "Status: Idle");
    lv_obj_align(s_recording_status, LV_ALIGN_TOP_LEFT, 10, 140);
    
    lv_obj_t *record_btn = lv_btn_create(s_debug_panel);
    if (record_btn == NULL) {
        ESP_LOGE(TAG, "Failed to create record button - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_obj_set_size(record_btn, 100, 40);
    lv_obj_align(record_btn, LV_ALIGN_TOP_LEFT, 10, 170);
    
    lv_obj_t *record_label = lv_label_create(record_btn);
    if (record_label == NULL) {
        ESP_LOGE(TAG, "Failed to create record label - out of memory");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_label_set_text(record_label, "Record");
    lv_obj_center(record_label);
    
    lv_obj_t *play_recorded_btn = lv_btn_create(s_debug_panel);
    lv_obj_set_size(play_recorded_btn, 100, 40);
    lv_obj_align(play_recorded_btn, LV_ALIGN_TOP_LEFT, 120, 170);
    
    lv_obj_t *play_label = lv_label_create(play_recorded_btn);
    lv_label_set_text(play_label, "Play");
    lv_obj_center(play_label);
    
    lv_obj_t *audio_section = lv_label_create(s_debug_panel);
    lv_label_set_text(audio_section, "Audio Playback Test:");
    lv_obj_align(audio_section, LV_ALIGN_TOP_LEFT, 10, 230);
    
    s_playing_status = lv_label_create(s_debug_panel);
    lv_label_set_text(s_playing_status, "Playing: No");
    lv_obj_align(s_playing_status, LV_ALIGN_TOP_LEFT, 10, 260);
    
    lv_obj_t *test_audio_btn = lv_btn_create(s_debug_panel);
    lv_obj_set_size(test_audio_btn, 100, 40);
    lv_obj_align(test_audio_btn, LV_ALIGN_TOP_LEFT, 10, 290);
    
    lv_obj_t *test_label = lv_label_create(test_audio_btn);
    lv_label_set_text(test_label, "Test Audio");
    lv_obj_center(test_label);
    
    s_debug_status = lv_label_create(s_debug_panel);
    lv_label_set_text(s_debug_status, "Ready");
    lv_obj_align(s_debug_status, LV_ALIGN_TOP_LEFT, 10, 340);
    
    lv_obj_t *back_btn = lv_btn_create(s_debug_panel);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);
    
    s_ui_initialized = true;
    lvgl_port_unlock();

    ESP_LOGI(TAG, "UI initialized");
    return ESP_OK;

fail:
    lvgl_port_unlock();
    return ret;
}

esp_err_t ui_show_panel(ui_panel_t panel)
{
    if (!s_ui_initialized || s_main_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }
    
    lv_obj_add_flag(s_main_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_chat_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_settings_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_loading_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_debug_panel, LV_OBJ_FLAG_HIDDEN);
    
    switch (panel) {
        case UI_PANEL_MAIN:
            lv_obj_clear_flag(s_main_panel, LV_OBJ_FLAG_HIDDEN);
            break;
        case UI_PANEL_CHAT:
            lv_obj_clear_flag(s_chat_panel, LV_OBJ_FLAG_HIDDEN);
            break;
        case UI_PANEL_SETTINGS:
            lv_obj_clear_flag(s_settings_panel, LV_OBJ_FLAG_HIDDEN);
            break;
        case UI_PANEL_LOADING:
            lv_obj_clear_flag(s_loading_panel, LV_OBJ_FLAG_HIDDEN);
            break;
        case UI_PANEL_DEBUG:
            lv_obj_clear_flag(s_debug_panel, LV_OBJ_FLAG_HIDDEN);
            break;
        default:
            lvgl_port_unlock();
            return ESP_ERR_INVALID_ARG;
    }

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t ui_update_chat_message(const char *user_msg, const char *ai_msg)
{
    if (!s_ui_initialized || s_chat_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }
    
    lv_obj_clean(s_chat_panel);
    
    if (user_msg != NULL) {
        lv_obj_t *user_label = lv_label_create(s_chat_panel);
        lv_label_set_text_fmt(user_label, "You: %s", user_msg);
        lv_obj_set_style_text_align(user_label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(user_label, LV_ALIGN_TOP_LEFT, 10, 10);
    }
    
    if (ai_msg != NULL) {
        lv_obj_t *ai_label = lv_label_create(s_chat_panel);
        lv_label_set_text_fmt(ai_label, "AI: %s", ai_msg);
        lv_obj_set_style_text_align(ai_label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(ai_label, LV_ALIGN_TOP_LEFT, 10, 60);
    }

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t ui_update_status(const char *status)
{
    if (!s_ui_initialized || s_main_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }
    
    lv_obj_t *status_label = lv_obj_get_child(s_main_panel, 1);
    if (status_label != NULL) {
        lv_label_set_text(status_label, status);
    }

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t ui_update_provider(const char *provider_name)
{
    if (!s_ui_initialized || s_main_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }
    
    lv_obj_t *title = lv_obj_get_child(s_main_panel, 0);
    if (title != NULL) {
        lv_label_set_text_fmt(title, "AI Chat - %s", provider_name);
    }

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t ui_debug_update_mic_level(int level)
{
    if (!s_ui_initialized || s_mic_level_bar == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }
    
    level = (level < 0) ? 0 : (level > 100) ? 100 : level;
    
    lv_bar_set_value(s_mic_level_bar, level, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_mic_level_label, "Level: %d%%", level);

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t ui_debug_update_status(const char *status)
{
    if (!s_ui_initialized || s_debug_status == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }
    
    lv_label_set_text(s_debug_status, status);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t ui_debug_set_recording_state(bool recording)
{
    if (!s_ui_initialized || s_recording_status == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }
    
    if (recording) {
        lv_label_set_text(s_recording_status, "Status: Recording...");
        lv_obj_set_style_text_color(s_recording_status, lv_palette_main(LV_PALETTE_RED), 0);
    } else {
        lv_label_set_text(s_recording_status, "Status: Idle");
        lv_obj_set_style_text_color(s_recording_status, lv_color_white(), 0);
    }

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t ui_debug_set_playing_state(bool playing)
{
    if (!s_ui_initialized || s_playing_status == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }
    
    if (playing) {
        lv_label_set_text(s_playing_status, "Playing: Yes");
        lv_obj_set_style_text_color(s_playing_status, lv_palette_main(LV_PALETTE_GREEN), 0);
    } else {
        lv_label_set_text(s_playing_status, "Playing: No");
        lv_obj_set_style_text_color(s_playing_status, lv_color_white(), 0);
    }

    lvgl_port_unlock();
    return ESP_OK;
}

void ui_task(void)
{
    if (!s_ui_initialized) {
        return;
    }

    if (lvgl_port_lock(0)) {
        lv_task_handler();
        lvgl_port_unlock();
    }
}
