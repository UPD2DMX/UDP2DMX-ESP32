set(COMPONENT_SRCS 
    "src/main.c"
    "src/dmx_manager.c"
    "src/dmx_status_http.c"
    "src/udp_protocol.c"
    "src/udp_server.c"
    "src/system_config.c"
)

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS "include" "."
    PRIV_REQUIRES esp_event esp_netif driver nvs_flash esp_dmx esp_wifi esp_eth my_wifi my_led my_config config_handler my_ethernet
)

message(STATUS "main component with modular structure included")
