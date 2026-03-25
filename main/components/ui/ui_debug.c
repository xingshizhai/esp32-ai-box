#include "ui.h"
#include "ui_debug_internal.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "ui_debug";

static lv_obj_t *s_debug_menu_view = NULL;
static lv_obj_t *s_debug_mic_view = NULL;
static lv_obj_t *s_debug_play_view = NULL;
#if CONFIG_SDCARD_ENABLED
static lv_obj_t *s_debug_sd_card_view = NULL;
static lv_obj_t *s_debug_sd_mp3_view = NULL;
#endif

static lv_obj_t *s_mic_level_bar = NULL;
static lv_obj_t *s_mic_level_label = NULL;
static lv_obj_t *s_recording_status = NULL;
#if CONFIG_SDCARD_ENABLED
static lv_obj_t *s_debug_sd_card_status = NULL;
static lv_obj_t *s_debug_sd_mp3_status = NULL;
#endif
static lv_obj_t *s_playing_status = NULL;
static lv_obj_t *s_play_status_label = NULL;
static lv_obj_t *s_play_volume_slider = NULL;
static lv_obj_t *s_play_volume_label = NULL;

static ui_debug_action_callback_t s_record_action_cb = NULL;
static ui_debug_action_callback_t s_play_record_action_cb = NULL;
static ui_debug_action_callback_t s_play_action_cb = NULL;
static ui_debug_volume_callback_t s_play_volume_cb = NULL;
#if CONFIG_SDCARD_ENABLED
static ui_debug_action_callback_t s_sdcard_action_cb = NULL;
static ui_debug_action_callback_t s_test_audio_action_cb = NULL;
#endif

typedef enum {
    DEBUG_VIEW_MENU = 0,
    DEBUG_VIEW_MIC,
    DEBUG_VIEW_PLAY,
    DEBUG_VIEW_SD_CARD,
    DEBUG_VIEW_SD_MP3,
} debug_view_t;

static const char *ui_debug_event_code_to_str(lv_event_code_t code)
{
    switch (code) {
        case LV_EVENT_PRESSED:
            return "PRESSED";
        case LV_EVENT_PRESSING:
            return "PRESSING";
        case LV_EVENT_PRESS_LOST:
            return "PRESS_LOST";
        case LV_EVENT_SHORT_CLICKED:
            return "SHORT_CLICKED";
        case LV_EVENT_CLICKED:
            return "CLICKED";
        case LV_EVENT_RELEASED:
            return "RELEASED";
        default:
            return "OTHER";
    }
}

static bool ui_debug_is_activate_event(lv_event_code_t code)
{
    return code == LV_EVENT_CLICKED;
}

static void ui_debug_show_view_locked(debug_view_t view)
{
    if (s_debug_menu_view == NULL) {
        return;
    }

    lv_obj_add_flag(s_debug_menu_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_debug_mic_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_debug_play_view, LV_OBJ_FLAG_HIDDEN);
#if CONFIG_SDCARD_ENABLED
    lv_obj_add_flag(s_debug_sd_card_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_debug_sd_mp3_view, LV_OBJ_FLAG_HIDDEN);
#endif

    switch (view) {
        case DEBUG_VIEW_MENU:
            lv_obj_clear_flag(s_debug_menu_view, LV_OBJ_FLAG_HIDDEN);
            break;
        case DEBUG_VIEW_MIC:
            lv_obj_clear_flag(s_debug_mic_view, LV_OBJ_FLAG_HIDDEN);
            break;
        case DEBUG_VIEW_PLAY:
            lv_obj_clear_flag(s_debug_play_view, LV_OBJ_FLAG_HIDDEN);
            break;
#if CONFIG_SDCARD_ENABLED
        case DEBUG_VIEW_SD_CARD:
            lv_obj_clear_flag(s_debug_sd_card_view, LV_OBJ_FLAG_HIDDEN);
            break;
        case DEBUG_VIEW_SD_MP3:
            lv_obj_clear_flag(s_debug_sd_mp3_view, LV_OBJ_FLAG_HIDDEN);
            break;
#endif
        default:
            lv_obj_clear_flag(s_debug_menu_view, LV_OBJ_FLAG_HIDDEN);
            break;
    }
}

void ui_debug_show_menu_view_locked(void)
{
    ui_debug_show_view_locked(DEBUG_VIEW_MENU);
}

static void ui_debug_menu_mic_event_cb(lv_event_t *event)
{
    if (ui_debug_is_activate_event(lv_event_get_code(event))) {
        ui_debug_show_view_locked(DEBUG_VIEW_MIC);
    }
}

static void ui_debug_menu_play_event_cb(lv_event_t *event)
{
    if (ui_debug_is_activate_event(lv_event_get_code(event))) {
        ui_debug_show_view_locked(DEBUG_VIEW_PLAY);
    }
}

#if CONFIG_SDCARD_ENABLED
static void ui_debug_menu_sd_card_event_cb(lv_event_t *event)
{
    if (ui_debug_is_activate_event(lv_event_get_code(event))) {
        ui_debug_show_view_locked(DEBUG_VIEW_SD_CARD);
    }
}

static void ui_debug_menu_sd_mp3_event_cb(lv_event_t *event)
{
    if (ui_debug_is_activate_event(lv_event_get_code(event))) {
        ui_debug_show_view_locked(DEBUG_VIEW_SD_MP3);
    }
}
#endif

static void ui_debug_submenu_back_event_cb(lv_event_t *event)
{
    if (ui_debug_is_activate_event(lv_event_get_code(event))) {
        ui_debug_show_view_locked(DEBUG_VIEW_MENU);
    }
}

static void ui_record_btn_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        ESP_LOGI(TAG, "Record button event: %s", ui_debug_event_code_to_str(code));
    }

    /* Use PRESSED here to avoid missing CLICKED on touch jitter and keep one-shot behavior. */
    if (code == LV_EVENT_PRESSED && s_record_action_cb != NULL) {
        s_record_action_cb();
    }
}

static void ui_play_btn_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        ESP_LOGI(TAG, "Play button event: %s", ui_debug_event_code_to_str(code));
    }

    /* Use PRESSED here to avoid missing CLICKED on touch jitter and keep one-shot behavior. */
    if (code == LV_EVENT_PRESSED && s_play_action_cb != NULL) {
        s_play_action_cb();
    }
}

static void ui_play_record_btn_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        ESP_LOGI(TAG, "Play-Record button event: %s", ui_debug_event_code_to_str(code));
    }

    if (code == LV_EVENT_PRESSED && s_play_record_action_cb != NULL) {
        s_play_record_action_cb();
    }
}

static void ui_play_volume_slider_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    if (s_play_volume_slider == NULL) {
        return;
    }

    int volume = lv_slider_get_value(s_play_volume_slider);
    if (s_play_volume_label != NULL) {
        lv_label_set_text_fmt(s_play_volume_label, "Vol %d", volume);
    }
    if (s_play_volume_cb != NULL) {
        s_play_volume_cb(volume);
    }
}

#if CONFIG_SDCARD_ENABLED
static void ui_test_audio_btn_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        ESP_LOGI(TAG, "Test Audio button event: %s", ui_debug_event_code_to_str(code));
    }

    if (ui_debug_is_activate_event(code) && s_test_audio_action_cb != NULL) {
        s_test_audio_action_cb();
    }
}

static void ui_sd_card_btn_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        ESP_LOGI(TAG, "SD Card button event: %s", ui_debug_event_code_to_str(code));
    }

    if (ui_debug_is_activate_event(code) && s_sdcard_action_cb != NULL) {
        s_sdcard_action_cb();
    }
}
#endif /* CONFIG_SDCARD_ENABLED */

esp_err_t ui_debug_init_views(lv_obj_t *debug_panel, const lv_style_t *style, lv_event_cb_t back_to_main_cb)
{
    if (debug_panel == NULL || style == NULL || back_to_main_cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_debug_menu_view = lv_obj_create(debug_panel);
    if (s_debug_menu_view == NULL) {
        ESP_LOGE(TAG, "Failed to create debug menu view - out of memory");
        return ESP_ERR_NO_MEM;
    }
    lv_obj_add_style(s_debug_menu_view, style, 0);
    lv_obj_set_size(s_debug_menu_view, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(s_debug_menu_view);
    lv_obj_clear_flag(s_debug_menu_view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *menu_title = lv_label_create(s_debug_menu_view);
    if (menu_title == NULL) {
        ESP_LOGE(TAG, "Failed to create debug menu title - out of memory");
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_text(menu_title, "Debug Menu");
    lv_obj_align(menu_title, LV_ALIGN_TOP_MID, 0, 10);

    const int split_btn_width = 126;
    const int split_btn_gap = 10;
    const int split_total_width = (split_btn_width * 2) + split_btn_gap;
    const int split_left_x = (LV_HOR_RES - split_total_width) / 2;
    const int menu_btn_height = 36;
    const int menu_row_1_y = 40;
#if CONFIG_SDCARD_ENABLED
    const int menu_row_2_y = 94;
#endif

    lv_obj_t *menu_mic_btn = lv_btn_create(s_debug_menu_view);
    lv_obj_set_size(menu_mic_btn, split_btn_width, menu_btn_height);
    lv_obj_align(menu_mic_btn, LV_ALIGN_TOP_LEFT, split_left_x, menu_row_1_y);
    lv_obj_add_event_cb(menu_mic_btn, ui_debug_menu_mic_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *menu_mic_label = lv_label_create(menu_mic_btn);
    lv_label_set_text(menu_mic_label, "Mic");
    lv_obj_center(menu_mic_label);

    lv_obj_t *menu_play_btn = lv_btn_create(s_debug_menu_view);
    lv_obj_set_size(menu_play_btn, split_btn_width, menu_btn_height);
    lv_obj_align(menu_play_btn, LV_ALIGN_TOP_LEFT, split_left_x + split_btn_width + split_btn_gap, menu_row_1_y);
    lv_obj_add_event_cb(menu_play_btn, ui_debug_menu_play_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *menu_play_label = lv_label_create(menu_play_btn);
    lv_label_set_text(menu_play_label, "Play");
    lv_obj_center(menu_play_label);

#if CONFIG_SDCARD_ENABLED
    lv_obj_t *menu_sd_card_btn = lv_btn_create(s_debug_menu_view);
    lv_obj_set_size(menu_sd_card_btn, split_btn_width, menu_btn_height);
    lv_obj_align(menu_sd_card_btn, LV_ALIGN_TOP_LEFT, split_left_x, menu_row_2_y);
    lv_obj_add_event_cb(menu_sd_card_btn, ui_debug_menu_sd_card_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *menu_sd_card_label = lv_label_create(menu_sd_card_btn);
    lv_label_set_text(menu_sd_card_label, "SD Card");
    lv_obj_center(menu_sd_card_label);

    lv_obj_t *menu_sd_mp3_btn = lv_btn_create(s_debug_menu_view);
    lv_obj_set_size(menu_sd_mp3_btn, split_btn_width, menu_btn_height);
    lv_obj_align(menu_sd_mp3_btn, LV_ALIGN_TOP_LEFT, split_left_x + split_btn_width + split_btn_gap, menu_row_2_y);
    lv_obj_add_event_cb(menu_sd_mp3_btn, ui_debug_menu_sd_mp3_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *menu_sd_mp3_label = lv_label_create(menu_sd_mp3_btn);
    lv_label_set_text(menu_sd_mp3_label, "SD MP3");
    lv_obj_center(menu_sd_mp3_label);
#endif /* CONFIG_SDCARD_ENABLED */

    lv_obj_t *menu_back_btn = lv_btn_create(s_debug_menu_view);
    lv_obj_set_size(menu_back_btn, 140, 34);
    lv_obj_align(menu_back_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_event_cb(menu_back_btn, back_to_main_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *menu_back_label = lv_label_create(menu_back_btn);
    lv_label_set_text(menu_back_label, "Back Main");
    lv_obj_center(menu_back_label);

    s_debug_mic_view = lv_obj_create(debug_panel);
    if (s_debug_mic_view == NULL) {
        ESP_LOGE(TAG, "Failed to create mic submenu - out of memory");
        return ESP_ERR_NO_MEM;
    }
    lv_obj_add_style(s_debug_mic_view, style, 0);
    lv_obj_set_size(s_debug_mic_view, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(s_debug_mic_view);
    lv_obj_clear_flag(s_debug_mic_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_debug_mic_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *mic_title = lv_label_create(s_debug_mic_view);
    lv_label_set_text(mic_title, "Microphone Test");
    lv_obj_align(mic_title, LV_ALIGN_TOP_MID, 0, 10);

    s_mic_level_bar = lv_bar_create(s_debug_mic_view);
    if (s_mic_level_bar == NULL) {
        ESP_LOGE(TAG, "Failed to create mic level bar - out of memory");
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(s_mic_level_bar, LV_HOR_RES - 40, 20);
    lv_obj_align(s_mic_level_bar, LV_ALIGN_TOP_MID, 0, 58);
    lv_bar_set_range(s_mic_level_bar, 0, 100);
    lv_bar_set_value(s_mic_level_bar, 0, LV_ANIM_OFF);

    s_mic_level_label = lv_label_create(s_debug_mic_view);
    if (s_mic_level_label == NULL) {
        ESP_LOGE(TAG, "Failed to create mic level label - out of memory");
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_text_fmt(s_mic_level_label, "Level: %d%%", 0);
    lv_obj_align(s_mic_level_label, LV_ALIGN_TOP_MID, 0, 88);

    s_recording_status = lv_label_create(s_debug_mic_view);
    if (s_recording_status == NULL) {
        ESP_LOGE(TAG, "Failed to create recording status - out of memory");
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_text(s_recording_status, "Status: Idle");
    lv_obj_align(s_recording_status, LV_ALIGN_TOP_MID, 0, 118);

    lv_obj_t *mic_back_btn = lv_btn_create(s_debug_mic_view);
    lv_obj_set_size(mic_back_btn, 140, 36);
    lv_obj_align(mic_back_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(mic_back_btn, ui_debug_submenu_back_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *mic_back_label = lv_label_create(mic_back_btn);
    lv_label_set_text(mic_back_label, "Back Menu");
    lv_obj_center(mic_back_label);

    lv_obj_t *mic_action_btn = lv_btn_create(s_debug_mic_view);
    lv_obj_set_size(mic_action_btn, 190, 40);
    lv_obj_align_to(mic_action_btn, mic_back_btn, LV_ALIGN_OUT_TOP_MID, 0, -12);
    lv_obj_add_event_cb(mic_action_btn, ui_record_btn_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *mic_action_label = lv_label_create(mic_action_btn);
    lv_label_set_text(mic_action_label, "Start / Stop Monitor");
    lv_obj_center(mic_action_label);

    s_debug_play_view = lv_obj_create(debug_panel);
    if (s_debug_play_view == NULL) {
        ESP_LOGE(TAG, "Failed to create playback submenu - out of memory");
        return ESP_ERR_NO_MEM;
    }
    lv_obj_add_style(s_debug_play_view, style, 0);
    lv_obj_set_size(s_debug_play_view, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(s_debug_play_view);
    lv_obj_clear_flag(s_debug_play_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_debug_play_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *play_title = lv_label_create(s_debug_play_view);
    lv_label_set_text(play_title, "Playback Test");
    lv_obj_align(play_title, LV_ALIGN_TOP_LEFT, 18, 12);

    s_playing_status = lv_label_create(s_debug_play_view);
    if (s_playing_status == NULL) {
        ESP_LOGE(TAG, "Failed to create playing status - out of memory");
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_text(s_playing_status, "Playing: No");
    lv_obj_align(s_playing_status, LV_ALIGN_TOP_RIGHT, -18, 12);

    s_play_status_label = lv_label_create(s_debug_play_view);
    if (s_play_status_label == NULL) {
        ESP_LOGE(TAG, "Failed to create playback status text - out of memory");
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(s_play_status_label, LV_HOR_RES - 28);
    lv_label_set_long_mode(s_play_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_play_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_play_status_label, "Ready");
    lv_obj_align(s_play_status_label, LV_ALIGN_TOP_MID, 0, 40);

    s_play_volume_label = lv_label_create(s_debug_play_view);
    if (s_play_volume_label == NULL) {
        ESP_LOGE(TAG, "Failed to create playback volume label - out of memory");
        return ESP_ERR_NO_MEM;
    }
    lv_label_set_text(s_play_volume_label, "Vol 80");
    lv_obj_align(s_play_volume_label, LV_ALIGN_TOP_LEFT, 18, 82);

    s_play_volume_slider = lv_slider_create(s_debug_play_view);
    if (s_play_volume_slider == NULL) {
        ESP_LOGE(TAG, "Failed to create playback volume slider - out of memory");
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(s_play_volume_slider, 145, 16);
    lv_obj_align(s_play_volume_slider, LV_ALIGN_TOP_RIGHT, -18, 84);
    lv_slider_set_range(s_play_volume_slider, 0, 100);
    lv_slider_set_value(s_play_volume_slider, 80, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_play_volume_slider, ui_play_volume_slider_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *play_back_btn = lv_btn_create(s_debug_play_view);
    lv_obj_set_size(play_back_btn, 140, 36);
    lv_obj_align(play_back_btn, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_event_cb(play_back_btn, ui_debug_submenu_back_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *play_back_label = lv_label_create(play_back_btn);
    lv_label_set_text(play_back_label, "Back Menu");
    lv_obj_center(play_back_label);

    lv_obj_t *play_play_btn = lv_btn_create(s_debug_play_view);
    lv_obj_set_size(play_play_btn, split_btn_width, 36);
    lv_obj_align(play_play_btn, LV_ALIGN_TOP_LEFT, split_left_x + split_btn_width + split_btn_gap, 124);
    lv_obj_add_event_cb(play_play_btn, ui_play_btn_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *play_play_label = lv_label_create(play_play_btn);
    lv_label_set_text(play_play_label, "Play Sample");
    lv_obj_center(play_play_label);

    lv_obj_t *play_record_btn = lv_btn_create(s_debug_play_view);
    lv_obj_set_size(play_record_btn, split_btn_width, 36);
    lv_obj_align(play_record_btn, LV_ALIGN_TOP_LEFT, split_left_x, 124);
    lv_obj_add_event_cb(play_record_btn, ui_play_record_btn_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *play_record_label = lv_label_create(play_record_btn);
    lv_label_set_text(play_record_label, "Record 5s");
    lv_obj_center(play_record_label);

#if CONFIG_SDCARD_ENABLED
    s_debug_sd_card_view = lv_obj_create(debug_panel);
    if (s_debug_sd_card_view == NULL) {
        ESP_LOGE(TAG, "Failed to create SD card submenu - out of memory");
        return ESP_ERR_NO_MEM;
    }
    lv_obj_add_style(s_debug_sd_card_view, style, 0);
    lv_obj_set_size(s_debug_sd_card_view, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(s_debug_sd_card_view);
    lv_obj_clear_flag(s_debug_sd_card_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_debug_sd_card_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *sd_card_title = lv_label_create(s_debug_sd_card_view);
    lv_label_set_text(sd_card_title, "SD Card Debug");
    lv_obj_align(sd_card_title, LV_ALIGN_TOP_MID, 0, 10);

    s_debug_sd_card_status = lv_label_create(s_debug_sd_card_view);
    if (s_debug_sd_card_status == NULL) {
        ESP_LOGE(TAG, "Failed to create SD card status - out of memory");
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(s_debug_sd_card_status, LV_HOR_RES - 24);
    lv_label_set_long_mode(s_debug_sd_card_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_debug_sd_card_status, "Ready");
    lv_obj_align(s_debug_sd_card_status, LV_ALIGN_TOP_MID, 0, 62);

    lv_obj_t *sd_card_action_btn = lv_btn_create(s_debug_sd_card_view);
    lv_obj_set_size(sd_card_action_btn, 190, 40);
    lv_obj_align(sd_card_action_btn, LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_add_event_cb(sd_card_action_btn, ui_sd_card_btn_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *sd_card_action_label = lv_label_create(sd_card_action_btn);
    lv_label_set_text(sd_card_action_label, "Check SD Card");
    lv_obj_center(sd_card_action_label);

    lv_obj_t *sd_card_back_btn = lv_btn_create(s_debug_sd_card_view);
    lv_obj_set_size(sd_card_back_btn, 140, 36);
    lv_obj_align(sd_card_back_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(sd_card_back_btn, ui_debug_submenu_back_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *sd_card_back_label = lv_label_create(sd_card_back_btn);
    lv_label_set_text(sd_card_back_label, "Back Menu");
    lv_obj_center(sd_card_back_label);

    s_debug_sd_mp3_view = lv_obj_create(debug_panel);
    if (s_debug_sd_mp3_view == NULL) {
        ESP_LOGE(TAG, "Failed to create SD MP3 submenu - out of memory");
        return ESP_ERR_NO_MEM;
    }
    lv_obj_add_style(s_debug_sd_mp3_view, style, 0);
    lv_obj_set_size(s_debug_sd_mp3_view, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(s_debug_sd_mp3_view);
    lv_obj_clear_flag(s_debug_sd_mp3_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_debug_sd_mp3_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *sd_mp3_title = lv_label_create(s_debug_sd_mp3_view);
    lv_label_set_text(sd_mp3_title, "Voice Roundtrip / SD MP3");
    lv_obj_align(sd_mp3_title, LV_ALIGN_TOP_MID, 0, 10);

    s_debug_sd_mp3_status = lv_label_create(s_debug_sd_mp3_view);
    if (s_debug_sd_mp3_status == NULL) {
        ESP_LOGE(TAG, "Failed to create SD MP3 status - out of memory");
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_width(s_debug_sd_mp3_status, LV_HOR_RES - 24);
    lv_label_set_long_mode(s_debug_sd_mp3_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_debug_sd_mp3_status, "Ready");
    lv_obj_align(s_debug_sd_mp3_status, LV_ALIGN_TOP_MID, 0, 62);

    lv_obj_t *sd_mp3_action_btn = lv_btn_create(s_debug_sd_mp3_view);
    lv_obj_set_size(sd_mp3_action_btn, 190, 40);
    lv_obj_align(sd_mp3_action_btn, LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_add_event_cb(sd_mp3_action_btn, ui_test_audio_btn_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *sd_mp3_action_label = lv_label_create(sd_mp3_action_btn);
    lv_label_set_text(sd_mp3_action_label, "Run Voice Test");
    lv_obj_center(sd_mp3_action_label);

    lv_obj_t *sd_mp3_back_btn = lv_btn_create(s_debug_sd_mp3_view);
    lv_obj_set_size(sd_mp3_back_btn, 140, 36);
    lv_obj_align(sd_mp3_back_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(sd_mp3_back_btn, ui_debug_submenu_back_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *sd_mp3_back_label = lv_label_create(sd_mp3_back_btn);
    lv_label_set_text(sd_mp3_back_label, "Back Menu");
    lv_obj_center(sd_mp3_back_label);
#endif /* CONFIG_SDCARD_ENABLED */

    return ESP_OK;
}

esp_err_t ui_debug_update_mic_level(int level)
{
    if (s_mic_level_bar == NULL || s_mic_level_label == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    static uint32_t s_lock_fail_count = 0;
    if (!lvgl_port_lock(10)) {
        s_lock_fail_count++;
        if ((s_lock_fail_count % 20) == 1) {
            ESP_LOGW(TAG, "Mic level UI update skipped due to LVGL lock timeout (%lu)", (unsigned long)s_lock_fail_count);
        }
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
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_recording_status == NULL && s_playing_status == NULL && s_play_status_label == NULL
#if CONFIG_SDCARD_ENABLED
        && s_debug_sd_card_status == NULL && s_debug_sd_mp3_status == NULL
#endif
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }

    if (s_recording_status != NULL) {
        lv_label_set_text(s_recording_status, status);
    }
    if (s_play_status_label != NULL) {
        lv_label_set_text(s_play_status_label, status);
    }

#if CONFIG_SDCARD_ENABLED
    if (s_debug_sd_card_status != NULL) {
        lv_label_set_text(s_debug_sd_card_status, status);
    }
    if (s_debug_sd_mp3_status != NULL) {
        lv_label_set_text(s_debug_sd_mp3_status, status);
    }
#endif

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t ui_debug_set_recording_state(bool recording)
{
    if (s_recording_status == NULL) {
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
    if (s_playing_status == NULL) {
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

esp_err_t ui_debug_set_play_volume(int volume)
{
    if (s_play_volume_slider == NULL || s_play_volume_label == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }

    lv_slider_set_value(s_play_volume_slider, volume, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_play_volume_label, "Vol %d", volume);

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t ui_debug_set_record_action_callback(ui_debug_action_callback_t callback)
{
    s_record_action_cb = callback;
    return ESP_OK;
}

esp_err_t ui_debug_set_play_record_action_callback(ui_debug_action_callback_t callback)
{
    s_play_record_action_cb = callback;
    return ESP_OK;
}

esp_err_t ui_debug_set_play_action_callback(ui_debug_action_callback_t callback)
{
    s_play_action_cb = callback;
    return ESP_OK;
}

esp_err_t ui_debug_set_play_volume_callback(ui_debug_volume_callback_t callback)
{
    s_play_volume_cb = callback;
    return ESP_OK;
}

#if CONFIG_SDCARD_ENABLED
esp_err_t ui_debug_set_sdcard_action_callback(ui_debug_action_callback_t callback)
{
    s_sdcard_action_cb = callback;
    return ESP_OK;
}

esp_err_t ui_debug_set_test_audio_action_callback(ui_debug_action_callback_t callback)
{
    s_test_audio_action_cb = callback;
    return ESP_OK;
}
#endif
