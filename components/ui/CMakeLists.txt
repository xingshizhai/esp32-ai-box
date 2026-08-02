idf_component_register(
    SRCS "ui.c" "ui_debug.c" "fonts/ui_font_zh_14.c" "fonts/cjk_font.c"
    INCLUDE_DIRS "." "fonts"
    REQUIRES lvgl esp_lvgl_port storage esp_timer
)
