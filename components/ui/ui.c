#include "ui.h"
#include "ui_debug_internal.h"
#include "cjk_font.h"
#include "ui_font_zh_14.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "storage.h"

#include <string.h>

static const char *TAG = "ui";

typedef enum {
    PET_MOOD_IDLE = 0,
    PET_MOOD_LISTEN,
    PET_MOOD_THINK,
    PET_MOOD_SPEAK,
    PET_MOOD_HAPPY,
    PET_MOOD_SAD,
    PET_MOOD_ERROR,
} pet_mood_t;

typedef struct {
    lv_obj_t *title_label;
    lv_obj_t *provider_label;
    lv_obj_t *status_label;
    lv_obj_t *hint_label;
    lv_obj_t *action_btn;
    lv_obj_t *action_label;
    lv_obj_t *debug_btn;
    lv_obj_t *face;
    lv_obj_t *eye_l;
    lv_obj_t *eye_r;
    lv_obj_t *pupil_l;
    lv_obj_t *pupil_r;
    lv_obj_t *mouth;
    pet_mood_t mood;
} ui_main_view_t;

static lv_obj_t *s_main_panel = NULL;
static lv_obj_t *s_chat_panel = NULL;
static lv_obj_t *s_settings_panel = NULL;
static lv_obj_t *s_loading_panel = NULL;
static lv_obj_t *s_debug_panel = NULL;
static bool s_ui_initialized = false;
static ui_main_action_callback_t s_main_action_cb = NULL;
static ui_main_view_t s_main_view = {0};

/* Compiled-in font only covers ~90 curated glyphs used by static UI labels.
 * Free-form AI chat text needs much broader CJK coverage; that font is too
 * large for the app partition, so it is loaded at runtime from the
 * "storage" SPIFFS partition instead. Falls back to the compiled-in font
 * if the SPIFFS asset is missing or fails to load. */
static lv_font_t *s_cjk_font = NULL;

static void ui_load_cjk_font(void)
{
    if (storage_spiffs_mount() != ESP_OK) {
        ESP_LOGW(TAG, "CJK font: SPIFFS unavailable, using built-in fallback font");
        return;
    }
    s_cjk_font = cjk_font_create("/spiffs/font_zh_gb2312_14.cjkf", &ui_font_zh_14);
    if (s_cjk_font == NULL) {
        ESP_LOGW(TAG, "CJK font: CJKF asset unavailable/invalid, using built-in fallback font");
        return;
    }
    ESP_LOGI(TAG, "CJK font: GB2312 on-demand font ready");
}

static const lv_font_t *ui_main_font(void)
{
    return (s_cjk_font != NULL) ? s_cjk_font : &ui_font_zh_14;
}

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

static void ui_set_circle(lv_obj_t *obj, int size, uint32_t color)
{
    lv_obj_set_size(obj, size, size);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void ui_apply_pet_mood_locked(pet_mood_t mood)
{
    if (s_main_view.face == NULL) {
        return;
    }

    s_main_view.mood = mood;

    uint32_t face_color = 0xFBBF24;
    int pupil_y = 0;
    int mouth_w = 36;
    int mouth_h = 10;
    uint32_t mouth_color = 0x7C2D12;

    switch (mood) {
        case PET_MOOD_LISTEN:
            face_color = 0x34D399;
            pupil_y = 4;
            mouth_w = 28;
            mouth_h = 8;
            break;
        case PET_MOOD_THINK:
            face_color = 0x60A5FA;
            pupil_y = -3;
            mouth_w = 18;
            mouth_h = 6;
            break;
        case PET_MOOD_SPEAK:
            face_color = 0xF472B6;
            pupil_y = 0;
            mouth_w = 30;
            mouth_h = 18;
            mouth_color = 0x9F1239;
            break;
        case PET_MOOD_HAPPY:
            face_color = 0xFBBF24;
            pupil_y = 2;
            mouth_w = 42;
            mouth_h = 14;
            break;
        case PET_MOOD_SAD:
        case PET_MOOD_ERROR:
            face_color = 0xFB7185;
            pupil_y = 5;
            mouth_w = 24;
            mouth_h = 6;
            mouth_color = 0x881337;
            break;
        case PET_MOOD_IDLE:
        default:
            break;
    }

    lv_obj_set_style_bg_color(s_main_view.face, lv_color_hex(face_color), 0);
    lv_obj_align(s_main_view.pupil_l, LV_ALIGN_CENTER, -2, pupil_y);
    lv_obj_align(s_main_view.pupil_r, LV_ALIGN_CENTER, 2, pupil_y);
    lv_obj_set_size(s_main_view.mouth, mouth_w, mouth_h);
    lv_obj_set_style_bg_color(s_main_view.mouth, lv_color_hex(mouth_color), 0);
    lv_obj_set_style_radius(s_main_view.mouth, mouth_h / 2, 0);
    lv_obj_align(s_main_view.mouth, LV_ALIGN_BOTTOM_MID, 0, -18);
}

static pet_mood_t ui_mood_from_status(const char *status)
{
    if (status == NULL || status[0] == '\0') {
        return PET_MOOD_IDLE;
    }

    if (strstr(status, "Listen") || strstr(status, "聆听") || strstr(status, "Recording") ||
        strstr(status, "录音") || strstr(status, "Wake")) {
        return PET_MOOD_LISTEN;
    }
    if (strstr(status, "Think") || strstr(status, "思考") || strstr(status, "Process") ||
        strstr(status, "Recogn") || strstr(status, "识别")) {
        return PET_MOOD_THINK;
    }
    if (strstr(status, "Speak") || strstr(status, "说话") || strstr(status, "TTS") ||
        strstr(status, "Playing") || strstr(status, "播放")) {
        return PET_MOOD_SPEAK;
    }
    if (strstr(status, "Error") || strstr(status, "错误") || strstr(status, "Fail") ||
        strstr(status, "断") || strstr(status, "offline")) {
        return PET_MOOD_ERROR;
    }
    if (strstr(status, "Connected") || strstr(status, "已连接") || strstr(status, "Ready") ||
        strstr(status, "待机")) {
        return PET_MOOD_HAPPY;
    }

    return PET_MOOD_IDLE;
}

static void ui_apply_status_copy_locked(const char *status)
{
    if (s_main_view.hint_label == NULL || s_main_view.action_label == NULL ||
        s_main_view.action_btn == NULL) {
        return;
    }

    pet_mood_t mood = ui_mood_from_status(status);
    const char *hint = "点击和我聊聊天";
    const char *action = "开始对话";
    uint32_t action_color = 0x2563EB;

    switch (mood) {
        case PET_MOOD_LISTEN:
            hint = "聆听中";
            action = "聆听中";
            action_color = 0x059669;
            break;
        case PET_MOOD_THINK:
            hint = "思考中";
            action = "请稍候...";
            action_color = 0x2563EB;
            break;
        case PET_MOOD_SPEAK:
            hint = "说话中";
            action = "播放中";
            action_color = 0xDB2777;
            break;
        case PET_MOOD_ERROR:
        case PET_MOOD_SAD:
            hint = "错误，请返回调试";
            action = "返回调试";
            action_color = 0xE11D48;
            break;
        case PET_MOOD_HAPPY:
            hint = "已连接，点击开始对话";
            break;
        case PET_MOOD_IDLE:
        default:
            break;
    }

    lv_label_set_text(s_main_view.hint_label, hint);
    lv_label_set_text(s_main_view.action_label, action);
    lv_obj_set_style_bg_color(s_main_view.action_btn, lv_color_hex(action_color), 0);
}

static void ui_action_button_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED ||
        code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        ESP_LOGI(TAG, "Action button event: %s", ui_event_code_to_str(code));
    }

    if (ui_is_activate_event(code) && s_main_action_cb != NULL) {
        s_main_action_cb();
    }
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
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED ||
        code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
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
    if (ui_is_activate_event(code)) {
        ESP_LOGI(TAG, "Debug button activated, entering debug panel");
        (void)ui_show_panel_locked(UI_PANEL_DEBUG);
    }
}

static void ui_back_btn_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (ui_is_activate_event(code)) {
        ESP_LOGI(TAG, "Back button activated, returning to main panel");
        (void)ui_show_panel_locked(UI_PANEL_MAIN);
    }
}

static esp_err_t ui_build_pet_face(lv_obj_t *parent)
{
    s_main_view.face = lv_obj_create(parent);
    if (s_main_view.face == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ui_set_circle(s_main_view.face, 126, 0xFBBF24);
    lv_obj_set_style_shadow_width(s_main_view.face, 18, 0);
    lv_obj_set_style_shadow_opa(s_main_view.face, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(s_main_view.face, lv_color_hex(0xF59E0B), 0);
    lv_obj_align(s_main_view.face, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_clear_flag(s_main_view.face, LV_OBJ_FLAG_CLICKABLE);

    s_main_view.eye_l = lv_obj_create(s_main_view.face);
    s_main_view.eye_r = lv_obj_create(s_main_view.face);
    if (s_main_view.eye_l == NULL || s_main_view.eye_r == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ui_set_circle(s_main_view.eye_l, 28, 0xFFFFFF);
    ui_set_circle(s_main_view.eye_r, 28, 0xFFFFFF);
    lv_obj_align(s_main_view.eye_l, LV_ALIGN_CENTER, -26, -12);
    lv_obj_align(s_main_view.eye_r, LV_ALIGN_CENTER, 26, -12);

    s_main_view.pupil_l = lv_obj_create(s_main_view.eye_l);
    s_main_view.pupil_r = lv_obj_create(s_main_view.eye_r);
    if (s_main_view.pupil_l == NULL || s_main_view.pupil_r == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ui_set_circle(s_main_view.pupil_l, 12, 0x111827);
    ui_set_circle(s_main_view.pupil_r, 12, 0x111827);
    lv_obj_align(s_main_view.pupil_l, LV_ALIGN_CENTER, -2, 0);
    lv_obj_align(s_main_view.pupil_r, LV_ALIGN_CENTER, 2, 0);

    s_main_view.mouth = lv_obj_create(s_main_view.face);
    if (s_main_view.mouth == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_size(s_main_view.mouth, 36, 10);
    lv_obj_set_style_radius(s_main_view.mouth, 5, 0);
    lv_obj_set_style_bg_color(s_main_view.mouth, lv_color_hex(0x7C2D12), 0);
    lv_obj_set_style_bg_opa(s_main_view.mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_main_view.mouth, 0, 0);
    lv_obj_clear_flag(s_main_view.mouth, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_main_view.mouth, LV_ALIGN_BOTTOM_MID, 0, -18);

    ui_apply_pet_mood_locked(PET_MOOD_IDLE);
    return ESP_OK;
}

esp_err_t ui_init(void)
{
    ESP_LOGI(TAG, "Initializing UI");

    esp_err_t ret = ESP_OK;

    ui_load_cjk_font();

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
    lv_style_set_bg_color(&style, lv_color_hex(0x0B1220));
    lv_style_set_text_color(&style, lv_color_white());
    lv_style_set_text_font(&style, ui_main_font());

    s_main_panel = lv_obj_create(lv_scr_act());
    if (s_main_panel == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_obj_add_style(s_main_panel, &style, 0);
    lv_obj_set_size(s_main_panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(s_main_panel);
    lv_obj_clear_flag(s_main_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_main_panel, 0, 0);
    lv_obj_set_style_border_width(s_main_panel, 0, 0);
    lv_obj_add_event_cb(s_main_panel, ui_main_panel_event_cb, LV_EVENT_ALL, NULL);

    /* Soft radial-ish backdrop using stacked panels */
    lv_obj_t *glow = lv_obj_create(s_main_panel);
    if (glow == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    ui_set_circle(glow, 180, 0x1E3A5F);
    lv_obj_set_style_bg_opa(glow, LV_OPA_40, 0);
    lv_obj_align(glow, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_clear_flag(glow, LV_OBJ_FLAG_CLICKABLE);

    s_main_view.title_label = lv_label_create(s_main_panel);
    if (s_main_view.title_label == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_label_set_text(s_main_view.title_label, "AI Pet");
    lv_obj_set_style_text_font(s_main_view.title_label, ui_main_font(), 0);
    lv_obj_set_style_text_color(s_main_view.title_label, lv_color_hex(0xE2E8F0), 0);
    lv_obj_align(s_main_view.title_label, LV_ALIGN_TOP_LEFT, 12, 8);

    s_main_view.provider_label = lv_label_create(s_main_panel);
    if (s_main_view.provider_label == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_label_set_text(s_main_view.provider_label, "Provider: -");
    lv_obj_set_style_text_font(s_main_view.provider_label, ui_main_font(), 0);
    lv_obj_set_style_text_color(s_main_view.provider_label, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(s_main_view.provider_label, LV_ALIGN_TOP_RIGHT, -52, 8);

    ret = ui_build_pet_face(s_main_panel);
    if (ret != ESP_OK) {
        goto fail;
    }

    s_main_view.status_label = lv_label_create(s_main_panel);
    if (s_main_view.status_label == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_label_set_text(s_main_view.status_label, "待机中");
    lv_obj_set_style_text_font(s_main_view.status_label, ui_main_font(), 0);
    lv_obj_set_style_text_color(s_main_view.status_label, lv_color_hex(0xF8FAFC), 0);
    lv_obj_align(s_main_view.status_label, LV_ALIGN_TOP_MID, 0, 176);

    s_main_view.hint_label = lv_label_create(s_main_panel);
    if (s_main_view.hint_label == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_label_set_text(s_main_view.hint_label, "点击开始对话");
    lv_obj_set_style_text_font(s_main_view.hint_label, ui_main_font(), 0);
    lv_obj_set_style_text_color(s_main_view.hint_label, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(s_main_view.hint_label, LV_ALIGN_TOP_MID, 0, 196);
    /* The 240 px display only has room for one status line above the CTA. */
    lv_obj_add_flag(s_main_view.hint_label, LV_OBJ_FLAG_HIDDEN);

    s_main_view.action_btn = lv_btn_create(s_main_panel);
    if (s_main_view.action_btn == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_obj_set_size(s_main_view.action_btn, 168, 34);
    lv_obj_align(s_main_view.action_btn, LV_ALIGN_BOTTOM_MID, -20, -10);
    lv_obj_add_event_cb(s_main_view.action_btn, ui_action_button_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_style_bg_color(s_main_view.action_btn, lv_color_hex(0x2563EB), 0);
    lv_obj_set_style_bg_grad_color(s_main_view.action_btn, lv_color_hex(0x7C3AED), 0);
    lv_obj_set_style_bg_grad_dir(s_main_view.action_btn, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_radius(s_main_view.action_btn, 16, 0);
    lv_obj_set_style_shadow_width(s_main_view.action_btn, 0, 0);

    s_main_view.action_label = lv_label_create(s_main_view.action_btn);
    if (s_main_view.action_label == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_label_set_text(s_main_view.action_label, "开始对话");
    lv_obj_set_style_text_font(s_main_view.action_label, ui_main_font(), 0);
    lv_obj_center(s_main_view.action_label);

    s_main_view.debug_btn = lv_btn_create(s_main_panel);
    if (s_main_view.debug_btn == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_obj_set_size(s_main_view.debug_btn, 48, 28);
    lv_obj_align(s_main_view.debug_btn, LV_ALIGN_BOTTOM_RIGHT, -10, -12);
    lv_obj_add_event_cb(s_main_view.debug_btn, ui_debug_btn_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_style_bg_color(s_main_view.debug_btn, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_width(s_main_view.debug_btn, 1, 0);
    lv_obj_set_style_border_color(s_main_view.debug_btn, lv_color_hex(0x475569), 0);
    lv_obj_set_style_radius(s_main_view.debug_btn, 12, 0);
    lv_obj_set_style_shadow_width(s_main_view.debug_btn, 0, 0);

    lv_obj_t *debug_label = lv_label_create(s_main_view.debug_btn);
    if (debug_label == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_label_set_text(debug_label, "调试");
    lv_obj_set_style_text_font(debug_label, ui_main_font(), 0);
    lv_obj_center(debug_label);

    s_chat_panel = lv_obj_create(lv_scr_act());
    if (s_chat_panel == NULL) {
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
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_label_set_text(loading_label, "请稍候...");
    lv_obj_set_style_text_font(loading_label, ui_main_font(), 0);
    lv_obj_align(loading_label, LV_ALIGN_CENTER, 0, 0);

    s_debug_panel = lv_obj_create(lv_scr_act());
    if (s_debug_panel == NULL) {
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

    lv_obj_t *back_btn = lv_btn_create(s_chat_panel);
    lv_obj_set_size(back_btn, 64, 28);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_add_event_cb(back_btn, ui_back_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "返回");
    lv_obj_set_style_text_font(back_label, ui_main_font(), 0);
    lv_obj_center(back_label);

    if (user_msg != NULL) {
        lv_obj_t *user_label = lv_label_create(s_chat_panel);
        lv_label_set_text_fmt(user_label, "用户: %s", user_msg);
        lv_obj_set_style_text_font(user_label, ui_main_font(), 0);
        lv_obj_set_width(user_label, LV_HOR_RES - 24);
        lv_label_set_long_mode(user_label, LV_LABEL_LONG_WRAP);
        lv_obj_align(user_label, LV_ALIGN_TOP_LEFT, 12, 48);
    }

    if (ai_msg != NULL) {
        lv_obj_t *ai_label = lv_label_create(s_chat_panel);
        lv_label_set_text_fmt(ai_label, "助手: %s", ai_msg);
        lv_obj_set_style_text_font(ai_label, ui_main_font(), 0);
        lv_obj_set_width(ai_label, LV_HOR_RES - 24);
        lv_label_set_long_mode(ai_label, LV_LABEL_LONG_WRAP);
        lv_obj_align(ai_label, LV_ALIGN_TOP_LEFT, 12, 110);
    }

    ui_apply_pet_mood_locked(PET_MOOD_SPEAK);
    (void)ui_show_panel_locked(UI_PANEL_CHAT);

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t ui_update_status(const char *status)
{
    if (!s_ui_initialized || s_main_panel == NULL || s_main_view.status_label == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }

    lv_label_set_text(s_main_view.status_label, (status != NULL) ? status : "");
    ui_apply_pet_mood_locked(ui_mood_from_status(status));
    ui_apply_status_copy_locked(status);

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t ui_update_provider(const char *provider_name)
{
    if (!s_ui_initialized || s_main_panel == NULL || s_main_view.provider_label == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }

    lv_label_set_text_fmt(s_main_view.provider_label,
                          "%s",
                          (provider_name != NULL && provider_name[0] != '\0') ? provider_name : "-");

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t ui_set_main_action_callback(ui_main_action_callback_t callback)
{
    s_main_action_cb = callback;
    return ESP_OK;
}

esp_err_t ui_set_role_text(const char *title, const char *action_label)
{
    if (!s_ui_initialized || s_main_view.title_label == NULL ||
        s_main_view.action_label == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }
    if (title != NULL && title[0] != '\0') {
        lv_label_set_text(s_main_view.title_label, title);
    }
    if (action_label != NULL && action_label[0] != '\0') {
        lv_label_set_text(s_main_view.action_label, action_label);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t ui_font_run_self_test(void)
{
    if (!s_ui_initialized || !s_cjk_font) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ui_update_chat_message(
        "你好，小智！中文显示测试：春夏秋冬，东南西北。",
        "常用汉字覆盖验证：人工智能聊天伴侣已经准备好了。"
        "数字 123，标点：，。！？；【】《》");
    if (err == ESP_OK) err = ui_show_panel(UI_PANEL_CHAT);
    ESP_LOGI(TAG, "CJK font self-test: %s", esp_err_to_name(err));
    return err;
}

void ui_font_log_stats(void)
{
    cjk_font_log_stats(s_cjk_font);
}

void ui_task(void)
{
    static int64_t last_font_stats_us;
    if (!s_ui_initialized) {
        return;
    }

    if (lvgl_port_lock(0)) {
        lv_task_handler();
        lvgl_port_unlock();
    }
    int64_t now = esp_timer_get_time();
    if (s_cjk_font && now - last_font_stats_us >= 60000000) {
        cjk_font_log_stats(s_cjk_font);
        last_font_stats_us = now;
    }
}
