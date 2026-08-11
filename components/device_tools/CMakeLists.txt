idf_component_register(
    SRCS "device_tools.c"
    INCLUDE_DIRS "."
    REQUIRES
        cjson
        audio
        board_iface
        log
)
