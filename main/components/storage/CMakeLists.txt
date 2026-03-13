idf_component_register(
    SRCS "storage.c"
    INCLUDE_DIRS "."
    REQUIRES
        fatfs
        esp_driver_sdmmc
)
