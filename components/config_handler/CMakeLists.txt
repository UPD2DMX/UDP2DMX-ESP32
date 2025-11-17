if("${CMAKE_PROJECT_NAME}" STREQUAL "bootloader")
    return()
endif()

idf_component_register(
    SRCS "config_handler.c" "wifi_rest_handler.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event spiffs esp_http_server json my_config my_wifi esp_wifi esp_netif esp_timer
)
message(STATUS "config_handler component being included")
