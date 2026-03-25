#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// DMX Manager Configuration
#define DMX_UNIVERSE_SIZE 512
#define DMX_FADE_INTERVAL_MS 30

// Command result types for better error handling
typedef enum {
    DMX_CMD_SUCCESS,
    DMX_CMD_ERROR_INVALID_CHANNEL,
    DMX_CMD_ERROR_INVALID_VALUE,
    DMX_CMD_ERROR_CONFIG_MISSING,
    DMX_CMD_ERROR_MEMORY,
    DMX_CMD_ERROR_TIMEOUT
} dmx_command_result_t;

// DMX Manager functions
esp_err_t dmx_manager_init(int uart_num, int tx_pin, int rx_pin, int en_pin);
void dmx_manager_deinit(void);
bool dmx_manager_is_initialized(void);
size_t dmx_manager_send(void);

// Channel operations
dmx_command_result_t dmx_set_channel(int channel, uint8_t value, int fade_ms);
dmx_command_result_t dmx_set_multi_channels(int start_channel, const uint8_t *values, int count, int fade_ms);
dmx_command_result_t dmx_set_rgb(int channel, uint8_t r, uint8_t g, uint8_t b, int fade_ms);
dmx_command_result_t dmx_set_tunable_white(int channel, uint8_t warm_white, uint8_t cold_white, int fade_ms);
dmx_command_result_t dmx_set_light_ct(int channel, int brightness_percent, int color_temp_k, int fade_ms);

// Utility functions
uint8_t dmx_get_channel_value(int channel);
bool dmx_is_channel_fading(int channel);
void dmx_stop_all_fades(void);

// Bounds checking
bool dmx_is_channel_valid(int channel, int count);

#ifdef __cplusplus
}
#endif
