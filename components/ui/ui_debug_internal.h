#pragma once

#include "lvgl.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_debug_init_views(lv_obj_t *debug_panel, const lv_style_t *style, lv_event_cb_t back_to_main_cb);
void ui_debug_show_menu_view_locked(void);

#ifdef __cplusplus
}
#endif
