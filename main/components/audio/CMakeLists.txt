idf_component_register(
    SRCS "audio.c"
    INCLUDE_DIRS "."
    REQUIRES 
        esp_codec_dev
        driver
)
