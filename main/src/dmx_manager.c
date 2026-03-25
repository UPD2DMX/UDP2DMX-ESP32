#include "dmx_manager.h"
#include "my_config.h"

#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_dmx.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "dmx_manager";

// DMX state management
static bool dmx_initialized = false;
static dmx_port_t dmx_port = DMX_NUM_1;
// esp_dmx expects slot 0 = start code, slots 1..512 = channels
static uint8_t dmx_data[DMX_UNIVERSE_SIZE + 1] = {0};
static SemaphoreHandle_t dmx_mutex = NULL;

// Fade state management
typedef struct
{
    bool active;
    uint8_t start_value;
    uint8_t target_value;
    int duration_ms;
    TickType_t start_time;
} fade_state_t;

static fade_state_t fade_states[DMX_UNIVERSE_SIZE + 1] = {0};
static TaskHandle_t fade_task_handle = NULL;

// Private function declarations
static void fade_task(void *arg);
static bool is_array_index_valid(int index);
static dmx_command_result_t start_fade(int channel, uint8_t value, int duration_ms);
static void stop_fade(int channel);

// Bounds checking functions
bool dmx_is_channel_valid(int channel, int count)
{
    if (count < 1)
    {
        return false;
    }

    // Channels are 1..DMX_UNIVERSE_SIZE inclusive
    if (channel < 1 || channel > DMX_UNIVERSE_SIZE)
    {
        return false;
    }

    // Ensure end channel stays within range
    return (channel + count - 1) <= DMX_UNIVERSE_SIZE;
}

static bool is_array_index_valid(int index)
{
    // index 0 is start code, valid channel indexes are 1..DMX_UNIVERSE_SIZE
    return (index >= 1 && index <= DMX_UNIVERSE_SIZE);
}

// Initialize DMX manager
esp_err_t dmx_manager_init(int uart_num, int tx_pin, int rx_pin, int en_pin)
{
    if (dmx_initialized)
    {
        ESP_LOGW(TAG, "DMX manager already initialized");
        return ESP_OK;
    }

    // Create mutex for thread-safe operations
    dmx_mutex = xSemaphoreCreateMutex();
    if (dmx_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create DMX mutex");
        return ESP_ERR_NO_MEM;
    }

    // Select DMX UART/port from sdkconfig
    dmx_port_t configured_port = (dmx_port_t)uart_num;
    if (configured_port >= DMX_NUM_MAX)
    {
        ESP_LOGE(TAG, "Invalid UART/DMX port=%d (max port=%d)", uart_num, (int)DMX_NUM_MAX - 1);
        return ESP_ERR_INVALID_ARG;
    }
    dmx_port = configured_port;

    ESP_LOGI(TAG, "DMX init: port=%u tx=%d rx=%d en(rts)=%d baud=%d", (unsigned)dmx_port, tx_pin, rx_pin, en_pin, CONFIG_DMX_BAUDRATE);

    // Initialize DMX driver with default configuration
    dmx_config_t config = DMX_CONFIG_DEFAULT;

    if (!dmx_driver_install(dmx_port, &config, NULL, 0))
    {
        ESP_LOGE(TAG, "dmx_driver_install failed for port %u", (unsigned)dmx_port);
        return ESP_FAIL;
    }

    if (!dmx_set_pin(dmx_port, tx_pin, rx_pin, en_pin))
    {
        ESP_LOGE(TAG, "dmx_set_pin failed for port %u (tx=%d rx=%d rts=%d)", (unsigned)dmx_port, tx_pin, rx_pin, en_pin);
        return ESP_FAIL;
    }

    uint32_t actual_baud = dmx_set_baud_rate(dmx_port, CONFIG_DMX_BAUDRATE);
    if (actual_baud == 0)
    {
        ESP_LOGW(TAG, "dmx_set_baud_rate failed (requested=%d)", CONFIG_DMX_BAUDRATE);
    }
    else if (actual_baud != (uint32_t)CONFIG_DMX_BAUDRATE)
    {
        ESP_LOGW(TAG, "DMX baud clamped: requested=%d actual=%u", CONFIG_DMX_BAUDRATE, (unsigned)actual_baud);
    }

    // MAX1348 specific setup
    // Configure Enable pin for MAX1348 transceiver
    gpio_config_t en_pin_config = {
        .pin_bit_mask = (1ULL << en_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    esp_err_t gpio_err = gpio_config(&en_pin_config);
    if (gpio_err != ESP_OK)
    {
        ESP_LOGW(TAG, "gpio_config(en_pin=%d) failed: %s", en_pin, esp_err_to_name(gpio_err));
    }

    // Set MAX1348 to transmit mode (EN pin HIGH for TX mode)
    gpio_set_level(en_pin, 1);

    // Small delay for MAX1348 to settle
    vTaskDelay(pdMS_TO_TICKS(10));

    // Initialize data exactly like working code
    memset(dmx_data, 0, sizeof(dmx_data));
    memset(fade_states, 0, sizeof(fade_states));
    dmx_data[0] = DMX_SC; // start code
    size_t written = dmx_write(dmx_port, dmx_data, DMX_UNIVERSE_SIZE + 1);
    if (written != (size_t)(DMX_UNIVERSE_SIZE + 1))
    {
        ESP_LOGW(TAG, "dmx_write init wrote %u bytes (expected %u)", (unsigned)written, (unsigned)(DMX_UNIVERSE_SIZE + 1));
    }

    // Create fade task
    BaseType_t task_result = xTaskCreate(
        fade_task,
        "dmx_fade",
        4096,
        NULL,
        5,
        &fade_task_handle);

    if (task_result != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create fade task");
        // Continue anyway, like working code
    }

    dmx_initialized = true;
    ESP_LOGI(TAG, "DMX manager initialized successfully");

    return ESP_OK;
}

void dmx_manager_deinit(void)
{
    if (!dmx_initialized)
    {
        return;
    }

    // Stop fade task
    if (fade_task_handle != NULL)
    {
        vTaskDelete(fade_task_handle);
        fade_task_handle = NULL;
    }

    // Clean up DMX driver
    dmx_driver_delete(dmx_port);

    // Clean up mutex
    if (dmx_mutex != NULL)
    {
        vSemaphoreDelete(dmx_mutex);
        dmx_mutex = NULL;
    }

    dmx_initialized = false;
    ESP_LOGI(TAG, "DMX manager deinitialized");
}

bool dmx_manager_is_initialized(void)
{
    return dmx_initialized;
}

size_t dmx_manager_send(void)
{
    if (!dmx_initialized)
    {
        return 0;
    }
    return dmx_send(dmx_port);
}

// Set single channel
dmx_command_result_t dmx_set_channel(int channel, uint8_t value, int fade_ms)
{
    if (!dmx_initialized)
    {
        ESP_LOGE(TAG, "DMX manager not initialized");
        return DMX_CMD_ERROR_MEMORY;
    }

    if (!dmx_is_channel_valid(channel, 1))
    {
        ESP_LOGW(TAG, "Invalid channel: %d", channel);
        return DMX_CMD_ERROR_INVALID_CHANNEL;
    }

    int array_index = channel; // Use channel directly like original (bug compatibility)

    if (fade_ms > 0)
    {
        return start_fade(array_index, value, fade_ms);
    }
    else
    {
        stop_fade(array_index);

        if (xSemaphoreTake(dmx_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            dmx_data[array_index] = value;
            dmx_write(dmx_port, dmx_data, DMX_UNIVERSE_SIZE + 1);
            // Remove dmx_send() - only send in main loop like original
            xSemaphoreGive(dmx_mutex);
            return DMX_CMD_SUCCESS;
        }
        else
        {
            ESP_LOGW(TAG, "Failed to acquire mutex in dmx_set_channel");
            return DMX_CMD_ERROR_TIMEOUT;
        }
    }
}

// Set multiple channels
dmx_command_result_t dmx_set_multi_channels(int start_channel, const uint8_t *values, int count, int fade_ms)
{
    if (!dmx_initialized)
    {
        ESP_LOGE(TAG, "DMX manager not initialized");
        return DMX_CMD_ERROR_MEMORY;
    }

    if (!dmx_is_channel_valid(start_channel, count))
    {
        ESP_LOGW(TAG, "Invalid channel range: %d-%d", start_channel, start_channel + count - 1);
        return DMX_CMD_ERROR_INVALID_CHANNEL;
    }

    if (!values)
    {
        ESP_LOGW(TAG, "NULL values pointer in dmx_set_multi_channels");
        return DMX_CMD_ERROR_INVALID_VALUE;
    }

    int array_start = start_channel; // Use channel directly like original (bug compatibility)

    if (fade_ms > 0)
    {
        for (int i = 0; i < count; ++i)
        {
            dmx_command_result_t result = start_fade(array_start + i, values[i], fade_ms);
            if (result != DMX_CMD_SUCCESS)
            {
                return result;
            }
        }
        return DMX_CMD_SUCCESS;
    }
    else
    {
        if (xSemaphoreTake(dmx_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            for (int i = 0; i < count; ++i)
            {
                fade_states[array_start + i].active = false;
                dmx_data[array_start + i] = values[i];
            }
            dmx_write(dmx_port, dmx_data, DMX_UNIVERSE_SIZE + 1);
            // Remove dmx_send() - only send in main loop like original
            xSemaphoreGive(dmx_mutex);
            return DMX_CMD_SUCCESS;
        }
        else
        {
            ESP_LOGW(TAG, "Failed to acquire mutex in dmx_set_multi_channels");
            return DMX_CMD_ERROR_TIMEOUT;
        }
    }
}

// Set RGB channels
dmx_command_result_t dmx_set_rgb(int channel, uint8_t r, uint8_t g, uint8_t b, int fade_ms)
{
    uint8_t rgb_values[3] = {r, g, b};
    return dmx_set_multi_channels(channel, rgb_values, 3, fade_ms);
}

// Set tunable white channels
dmx_command_result_t dmx_set_tunable_white(int channel, uint8_t warm_white, uint8_t cold_white, int fade_ms)
{
    uint8_t tw_values[2] = {warm_white, cold_white};
    return dmx_set_multi_channels(channel, tw_values, 2, fade_ms);
}

// Set light with color temperature
dmx_command_result_t dmx_set_light_ct(int channel, int brightness_percent, int color_temp_k, int fade_ms)
{
    if (!dmx_initialized)
    {
        ESP_LOGE(TAG, "DMX manager not initialized");
        return DMX_CMD_ERROR_MEMORY;
    }

    // Clamp brightness to valid range
    brightness_percent = (brightness_percent < 0) ? 0 : (brightness_percent > 100) ? 100
                                                                                   : brightness_percent;

    // Get color temperature configuration
    int ct_ww, ct_cw, ch_ww, ch_cw;
    get_ct_sorted(channel, &ct_ww, &ct_cw, &ch_ww, &ch_cw);

    // Clamp color temperature to configured range
    if (color_temp_k < ct_ww)
        color_temp_k = ct_ww;
    if (color_temp_k > ct_cw)
        color_temp_k = ct_cw;

    // Calculate channel values
    uint8_t val_ww = 0, val_cw = 0;
    int brightness_255 = (brightness_percent * 255) / 100;

    if (color_temp_k <= ct_ww + 100)
    {
        // Almost pure warm white
        val_ww = brightness_255;
        val_cw = 0;
    }
    else if (color_temp_k >= ct_cw - 100)
    {
        // Almost pure cold white
        val_ww = 0;
        val_cw = brightness_255;
    }
    else
    {
        // Mixed color temperature
        int range = ct_cw - ct_ww;
        if (range <= 0)
        {
            ESP_LOGW(TAG, "Invalid CT range for channel %d", channel);
            return DMX_CMD_ERROR_CONFIG_MISSING;
        }

        long num_cw = (long)brightness_percent * (color_temp_k - ct_ww) * 255;
        long num_ww = (long)brightness_percent * (ct_cw - color_temp_k) * 255;
        long den = (long)range * 100;

        val_cw = (uint8_t)((num_cw + den / 2) / den);
        val_ww = (uint8_t)((num_ww + den / 2) / den);

        // Remove very low values (< 2%)
        if (val_cw * 100 / 255 < 2)
            val_cw = 0;
        if (val_ww * 100 / 255 < 2)
            val_ww = 0;
    }

    // Set channels based on which channel is lower
    int start_ch = (ch_ww < ch_cw) ? ch_ww : ch_cw;
    uint8_t values[2] = {0, 0};
    values[ch_ww - start_ch] = val_ww;
    values[ch_cw - start_ch] = val_cw;

    ESP_LOGI(TAG, "Light CT %dK, Brightness %d%% → WW=%d (CH%d), CW=%d (CH%d)",
             color_temp_k, brightness_percent, val_ww, ch_ww, val_cw, ch_cw);

    return dmx_set_multi_channels(start_ch, values, 2, fade_ms);
}

// Utility functions
uint8_t dmx_get_channel_value(int channel)
{
    if (!dmx_initialized || !dmx_is_channel_valid(channel, 1))
    {
        return 0;
    }

    uint8_t value = 0;
    int array_index = channel; // Use channel directly like original (bug compatibility)

    if (xSemaphoreTake(dmx_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        value = dmx_data[array_index];
        xSemaphoreGive(dmx_mutex);
    }

    return value;
}

bool dmx_is_channel_fading(int channel)
{
    if (!dmx_initialized || !dmx_is_channel_valid(channel, 1))
    {
        return false;
    }

    int array_index = channel; // Use channel directly like original (bug compatibility)
    bool fading = false;

    if (xSemaphoreTake(dmx_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        fading = fade_states[array_index].active;
        xSemaphoreGive(dmx_mutex);
    }

    return fading;
}

void dmx_stop_all_fades(void)
{
    if (!dmx_initialized)
    {
        return;
    }

    if (xSemaphoreTake(dmx_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        for (int i = 1; i <= DMX_UNIVERSE_SIZE; i++)
        {
            fade_states[i].active = false;
        }
        xSemaphoreGive(dmx_mutex);
    }
}

// Private functions
static dmx_command_result_t start_fade(int array_index, uint8_t value, int duration_ms)
{
    if (!is_array_index_valid(array_index))
    {
        ESP_LOGW(TAG, "Invalid array index for start_fade: %d", array_index);
        return DMX_CMD_ERROR_INVALID_CHANNEL;
    }

    if (xSemaphoreTake(dmx_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        fade_states[array_index].start_value = dmx_data[array_index];
        fade_states[array_index].target_value = value;
        fade_states[array_index].duration_ms = duration_ms;
        fade_states[array_index].start_time = xTaskGetTickCount();
        fade_states[array_index].active = true;
        xSemaphoreGive(dmx_mutex);
        return DMX_CMD_SUCCESS;
    }
    else
    {
        ESP_LOGW(TAG, "Failed to acquire mutex in start_fade");
        return DMX_CMD_ERROR_TIMEOUT;
    }
}

static void stop_fade(int array_index)
{
    if (!is_array_index_valid(array_index))
    {
        ESP_LOGW(TAG, "Invalid array index for stop_fade: %d", array_index);
        return;
    }

    if (xSemaphoreTake(dmx_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        fade_states[array_index].active = false;
        xSemaphoreGive(dmx_mutex);
    }
    else
    {
        ESP_LOGW(TAG, "Failed to acquire mutex in stop_fade");
    }
}

// Fade task implementation
static void fade_task(void *arg)
{
    ESP_LOGI(TAG, "DMX fade task started");

    while (1)
    {
        TickType_t now = xTaskGetTickCount();
        bool updated = false;

        if (xSemaphoreTake(dmx_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            for (int i = 1; i <= DMX_UNIVERSE_SIZE; ++i)
            {
                if (!fade_states[i].active)
                {
                    continue;
                }

                int elapsed = (now - fade_states[i].start_time) * portTICK_PERIOD_MS;
                int duration = fade_states[i].duration_ms;

                if (duration <= 0)
                {
                    dmx_data[i] = fade_states[i].target_value;
                    fade_states[i].active = false;
                    updated = true;
                    continue;
                }

                uint8_t start = fade_states[i].start_value;
                uint8_t target = fade_states[i].target_value;
                uint8_t new_value;

                if (elapsed >= duration)
                {
                    new_value = target;
                    fade_states[i].active = false;
                }
                else
                {
                    float t = (float)elapsed / (float)duration;
                    float interpolated = start + (target - start) * t;
                    new_value = (uint8_t)roundf(interpolated);
                }

                if (dmx_data[i] != new_value)
                {
                    dmx_data[i] = new_value;
                    updated = true;
                }
            }

            // Send DMX data if updated (write only when changed)
            if (updated)
            {
                dmx_write(dmx_port, dmx_data, DMX_UNIVERSE_SIZE + 1);
            }

            xSemaphoreGive(dmx_mutex);
        }
        else
        {
            ESP_LOGW(TAG, "Failed to acquire mutex in fade_task");
        }

        // Fade task timing - exact from working code
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
