idf_component_register(
    SRCS "ir_ctrl.c"
    INCLUDE_DIRS "."
    REQUIRES
        board_iface
        device_tools
        cjson
        nvs_flash
        esp_driver_rmt
        log
)
