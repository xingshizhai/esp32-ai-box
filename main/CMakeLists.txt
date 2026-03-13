cmake_minimum_required(VERSION 3.5)

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES 
        ai_service
        conversation
        config
        network
        ui
        audio
        storage
        esp_psram
        esp_adc
        esp_lcd
        esp_driver_i2c
        esp_driver_spi
        esp_driver_gpio
        esp_http_client
        cjson
)

add_compile_options(-fdiagnostics-color=always)
