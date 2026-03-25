#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int uart_num;
    int tx_pin;
    int rx_pin;
    int en_pin;
} rs485_port_config_t;

// System configuration
typedef struct {
    // Hardware pins
    struct {
        rs485_port_config_t rs485_out1;
        rs485_port_config_t rs485_out2;
        int dmx_output_select;  // 1 = rs485_out1, 2 = rs485_out2
        int debug_led_gpio;
    } hardware;
    
    // Ethernet configuration
    struct {
        bool enable;
        int mdc_gpio;
        int mdio_gpio;
        int phy_addr;
        int phy_power_gpio;
        int phy_rst_gpio;
        int clock_mode; // 0=GPIO0_IN, 1=GPIO0_OUT, 2=GPIO16_OUT, 3=GPIO17_OUT
    } ethernet;
    
    // Network configuration
    struct {
        uint16_t udp_port;
        uint16_t max_udp_buffer_size;
    } network;
    
    // DMX configuration
    struct {
        int universe_size;
        int fade_interval_ms;
    } dmx;
    
    // System settings
    struct {
        bool enable_debug_logging;
        int watchdog_timeout_ms;
    } system;
} system_config_t;

// Configuration functions
esp_err_t system_config_init(void);
const system_config_t* system_config_get(void);
esp_err_t system_config_load_from_nvs(void);
esp_err_t system_config_save_to_nvs(void);
esp_err_t system_config_load_defaults(void);
esp_err_t system_config_set_dmx_output_select(int output_select);
int system_config_get_dmx_output_select(void);
const rs485_port_config_t* system_config_get_active_dmx_port(void);

// Configuration validation
bool system_config_validate(const system_config_t* config);
void system_config_print(const system_config_t* config);

#ifdef __cplusplus
}
#endif
