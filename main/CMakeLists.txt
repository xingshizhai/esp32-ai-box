cmake_minimum_required(VERSION 3.5)

idf_component_register(
    SRCS "main.c"
         "app_runtime.c"
    INCLUDE_DIRS "."
    EMBED_FILES "assets/wake_ack.pcm"
    REQUIRES
        ai_service
        conversation
        config
        network
        ui
        debug_screenshot
        debug_input
        audio
        voice_chat
        storage
        board_iface
        role
        esp_psram
        esp_adc
        esp_lcd
        esp_driver_i2c
        esp_driver_spi
        esp_driver_gpio
        esp_driver_uart
        esp_http_client
        cjson
        espressif__esp-sr
)

add_compile_options(-fdiagnostics-color=always)
