idf_component_register(
    SRCS "src/board_display.c"
    REQUIRES
        board_iface
        log
    PRIV_REQUIRES
        esp_lcd
        espressif__esp_lvgl_port
        # TODO: add your BSP or LCD/touch driver component names here, e.g.:
        #   espressif__esp_lcd_touch_gt911
        #   espressif__esp_lcd_ili9341
)
