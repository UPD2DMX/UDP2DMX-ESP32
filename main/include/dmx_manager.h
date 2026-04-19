#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

// For timestamp queries (ms since boot)
#include "esp_timer.h"

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
esp_err_t dmx_manager_init(int tx_pin, int rx_pin, int en_pin);
void dmx_manager_deinit(void);
bool dmx_manager_is_initialized(void);

// Channel operations
dmx_command_result_t dmx_set_channel(int channel, uint8_t value, int fade_ms);
dmx_command_result_t dmx_set_multi_channels(int start_channel, const uint8_t *values, int count, int fade_ms);
dmx_command_result_t dmx_set_rgb(int channel, uint8_t r, uint8_t g, uint8_t b, int fade_ms);
dmx_command_result_t dmx_set_tunable_white(int channel, uint8_t warm_white, uint8_t cold_white, int fade_ms);
dmx_command_result_t dmx_set_light_ct(int channel, int brightness_percent, int color_temp_k, int fade_ms);

// Extended variant that optionally returns computed channel mapping and values.
// out_* pointers may be NULL.
dmx_command_result_t dmx_set_light_ct_ex(int channel, int brightness_percent, int color_temp_k, int fade_ms,
                                        int *out_ch_ww, int *out_ch_cw,
                                        uint8_t *out_val_ww, uint8_t *out_val_cw);

// Utility functions
uint8_t dmx_get_channel_value(int channel);
bool dmx_is_channel_fading(int channel);
void dmx_stop_all_fades(void);

// Diagnostics / status
// Returns milliseconds since boot when this channel was last changed by a command.
// 0 means "never" (since boot / init).
uint32_t dmx_get_channel_last_change_ms(int channel);
// Returns milliseconds since boot when ANY channel was last changed by a command.
// 0 means "never" (since boot / init).
uint32_t dmx_get_last_change_ms(void);

// Bounds checking
bool dmx_is_channel_valid(int channel, int count);

#ifdef __cplusplus
}
#endif
