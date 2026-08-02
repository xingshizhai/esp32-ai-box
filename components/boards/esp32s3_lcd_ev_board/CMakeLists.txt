idf_component_register(
    SRCS "src/board_display.c"
    REQUIRES
        board_iface
        log
    PRIV_REQUIRES
        esp_lcd
        espressif__esp_lvgl_port
        espressif__esp32_s3_lcd_ev_board
        espressif__esp_lcd_touch
        espressif__esp_io_expander
)
