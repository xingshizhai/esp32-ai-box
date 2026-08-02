idf_component_register(
    SRCS "config.c"
         "provider_catalog.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash
)
