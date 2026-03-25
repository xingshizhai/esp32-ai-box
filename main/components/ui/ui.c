#include "ui.h"
#include "ui_debug_internal.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "ui";

static lv_obj_t *s_main_panel = NULL;
static lv_obj_t *s_chat_panel = NULL;
static lv_obj_t *s_settings_panel = NULL;
static lv_obj_t *s_loading_panel = NULL;
static lv_obj_t *s_debug_panel = NULL;
static bool s_ui_initialized = false;
static ui_main_action_callback_t s_main_action_cb = NULL;

static const char *ui_event_code_to_str(lv_event_code_t code)
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

static const char *ui_panel_to_str(ui_panel_t panel)
{
    switch (panel) {
        case UI_PANEL_MAIN:
            return "MAIN";
        case UI_PANEL_CHAT:
            return "CHAT";
        case UI_PANEL_SETTINGS:
            return "SETTINGS";
        case UI_PANEL_LOADING:
            return "LOADING";
        case UI_PANEL_DEBUG:
            return "DEBUG";
        default:
            return "UNKNOWN";
    }
}

static bool ui_is_activate_event(lv_event_code_t code)
{
    return code == LV_EVENT_CLICKED;
}

static esp_err_t ui_show_panel_locked(ui_panel_t panel)
{
    ESP_LOGI(TAG, "Switch panel -> %s", ui_panel_to_str(panel));

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
            ui_debug_show_menu_view_locked();
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static void ui_main_panel_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        ESP_LOGI(TAG, "Main panel touch event: %s", ui_event_code_to_str(code));
    }

    if (ui_is_activate_event(code) && s_main_action_cb != NULL) {
        lv_obj_t *target = lv_event_get_target(event);
        lv_obj_t *current_target = lv_event_get_current_target(event);
        if (target == current_target) {
            s_main_action_cb();
        }
    }
}

static void ui_debug_btn_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        ESP_LOGI(TAG, "Debug button event: %s", ui_event_code_to_str(code));
    }

    if (ui_is_activate_event(code)) {
        ESP_LOGI(TAG, "Debug button activated, entering debug panel");
        (void)ui_show_panel_locked(UI_PANEL_DEBUG);
    }
}

static void ui_back_btn_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        ESP_LOGI(TAG, "Back button event: %s", ui_event_code_to_str(code));
    }

    if (ui_is_activate_event(code)) {
        ESP_LOGI(TAG, "Back button activated, returning to main panel");
        (void)ui_show_panel_locked(UI_PANEL_MAIN);
    }
}

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
    lv_obj_clear_flag(s_main_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_main_panel, ui_main_panel_event_cb, LV_EVENT_ALL, NULL);

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
    lv_obj_add_event_cb(debug_btn, ui_debug_btn_event_cb, LV_EVENT_ALL, NULL);

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
    lv_obj_clear_flag(s_chat_panel, LV_OBJ_FLAG_SCROLLABLE);
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
    lv_obj_clear_flag(s_settings_panel, LV_OBJ_FLAG_SCROLLABLE);
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
    lv_obj_clear_flag(s_loading_panel, LV_OBJ_FLAG_SCROLLABLE);
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
    lv_obj_clear_flag(s_debug_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_debug_panel, LV_OBJ_FLAG_HIDDEN);

    ret = ui_debug_init_views(s_debug_panel, &style, ui_back_btn_event_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize debug views (%s)", esp_err_to_name(ret));
        goto fail;
    }

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

    esp_err_t ret = ui_show_panel_locked(panel);

    lvgl_port_unlock();
    return ret;
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

esp_err_t ui_set_main_action_callback(ui_main_action_callback_t callback)
{
    s_main_action_cb = callback;
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
