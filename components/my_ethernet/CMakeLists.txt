idf_component_register(
    SRCS "my_ethernet.c"
    INCLUDE_DIRS "."
    REQUIRES esp_eth esp_netif mdns nvs_flash driver my_led
)
