idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        m5unified
        esp_http_server
        esp_netif
        esp_partition
        esp_wifi
        nvs_flash
        spiffs
)
