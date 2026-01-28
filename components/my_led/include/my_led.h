#pragma once

#include <stdbool.h>

/**
 * @brief Connection type enum
 */
typedef enum {
    CONNECTION_TYPE_NONE = 0,
    CONNECTION_TYPE_WIFI = 1,
    CONNECTION_TYPE_LAN = 2
} connection_type_t;

void my_led_init(int gpio);
void my_led_blink(int times, int delay_ms);
void my_led_set(bool on);
void my_led_set_wifi_status(bool connected);
void my_led_set_connection_type(connection_type_t type);
void my_led_set_dmx_error(bool error);
