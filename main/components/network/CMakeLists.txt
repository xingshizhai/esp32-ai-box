idf_component_register(
    SRCS "network.c"
    INCLUDE_DIRS "."
    REQUIRES
        config
        esp_wifi
        esp_event
        esp_netif
)
