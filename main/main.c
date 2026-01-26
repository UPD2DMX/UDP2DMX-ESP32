#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
// #include "protocol_examples_common.h"
#include "lwip/sockets.h"
#include "esp_dmx.h"
#include "driver/gpio.h"

#include "my_wifi.h"
#include "my_led.h"
#include "my_config.h"
#include "config_handler.h"
#include "system_config.h"

#ifdef CONFIG_ENABLE_ETHERNET
#include "my_ethernet.h"
#endif

#define DMX_UNIVERSE_SIZE DMX_PACKET_SIZE
#define UDP_PORT 6454
#define FADE_INTERVAL_MS 10
#define MAX_UDP_BUFFER_SIZE 1024

// Command result types for better error handling
typedef enum
{
    CMD_SUCCESS,
    CMD_ERROR_INVALID_CHANNEL,
    CMD_ERROR_INVALID_VALUE,
    CMD_ERROR_CONFIG_MISSING,
    CMD_ERROR_MEMORY
} command_result_t;

static const char *TAG = "udp_dmx";
static uint8_t dmx_data[DMX_UNIVERSE_SIZE] = {};
static dmx_port_t dmx_num = DMX_NUM_1;
static SemaphoreHandle_t dmx_mutex = NULL;

typedef struct
{
    bool active;
    int start_value;
    int target_value;
    int duration_ms;
    TickType_t start_time;
} fade_state_t;

static fade_state_t fade_states[DMX_UNIVERSE_SIZE] = {};

// Bounds checking function for channel validation
static bool is_channel_valid(int ch, int count)
{
    return (ch >= 1 && ch <= DMX_UNIVERSE_SIZE - count);
}

// Safe bounds checking for array access
static bool is_array_index_valid(int index)
{
    return (index >= 0 && index < DMX_UNIVERSE_SIZE);
}

// Convert speed_S to milliseconds (Using Loxone protocol)
static int speed_to_ms(int speed)
{
    if (speed == 255)
        return 0;
    if (speed >= 1 && speed <= 98)
        return speed * 591;
    if (speed >= 101 && speed <= 104)
        return (speed - 100) * 146 + 1;
    if (speed >= 201 && speed <= 254)
        return (speed - 200) * 72;
    if (speed == 254)
        return 3691;
    return 0;
}

static void stop_fade(int ch)
{
    if (!is_array_index_valid(ch))
    {
        ESP_LOGW(TAG, "Invalid channel for stop_fade: %d", ch);
        return;
    }

    if (xSemaphoreTake(dmx_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        fade_states[ch].active = false;
        xSemaphoreGive(dmx_mutex);
    }
    else
    {
        ESP_LOGW(TAG, "Failed to acquire mutex in stop_fade");
    }
}

static void start_fade(int ch, int value, int duration_ms)
{
    if (!is_array_index_valid(ch))
    {
        ESP_LOGW(TAG, "Invalid channel for start_fade: %d", ch);
        return;
    }

    // Clamp value to valid range
    value = (value < 0) ? 0 : (value > 255) ? 255
                                            : value;

    if (xSemaphoreTake(dmx_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        fade_states[ch].start_value = dmx_data[ch];
        fade_states[ch].target_value = value;
        fade_states[ch].duration_ms = duration_ms;
        fade_states[ch].start_time = xTaskGetTickCount();
        fade_states[ch].active = true;
        xSemaphoreGive(dmx_mutex);
    }
    else
    {
        ESP_LOGW(TAG, "Failed to acquire mutex in start_fade");
    }
}

static void fade_task(void *arg)
{
    while (1)
    {
        TickType_t now = xTaskGetTickCount();
        bool updated = false;

        if (xSemaphoreTake(dmx_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            for (int ch = 0; ch < DMX_UNIVERSE_SIZE; ++ch)
            {
                if (!fade_states[ch].active)
                    continue;

                int elapsed = (now - fade_states[ch].start_time) * portTICK_PERIOD_MS;
                int duration = fade_states[ch].duration_ms;

                if (duration <= 0)
                {
                    dmx_data[ch] = fade_states[ch].target_value;
                    fade_states[ch].active = false; // Use direct access since we have mutex
                    updated = true;
                    continue;
                }

                int start = fade_states[ch].start_value;
                int target = fade_states[ch].target_value;

                uint8_t new_value;

                if (elapsed >= duration)
                {
                    new_value = target;
                    fade_states[ch].active = false; // Use direct access since we have mutex
                }
                else
                {
                    float t = (float)elapsed / (float)duration;
                    float interpolated = start + (target - start) * t;
                    new_value = (uint8_t)roundf(interpolated);
                }

                if (dmx_data[ch] != new_value)
                {
                    dmx_data[ch] = new_value;
                    updated = true;
                }
            }

            if (updated)
            {
                esp_err_t err = dmx_write(dmx_num, dmx_data, DMX_UNIVERSE_SIZE);
                if (err != ESP_OK)
                {
                    ESP_LOGW(TAG, "DMX write failed: %s", esp_err_to_name(err));
                }
            }

            xSemaphoreGive(dmx_mutex);
        }
        else
        {
            ESP_LOGW(TAG, "Failed to acquire mutex in fade_task");
        }

        vTaskDelay(pdMS_TO_TICKS(FADE_INTERVAL_MS));
    }
}

static command_result_t set_channel(int ch, int value, int fade_ms)
{
    if (!is_channel_valid(ch, 1))
    {
        ESP_LOGW(TAG, "Invalid channel: %d", ch);
        return CMD_ERROR_INVALID_CHANNEL;
    }

    // Clamp value to valid range
    value = (value < 0) ? 0 : (value > 255) ? 255
                                            : value;

    if (fade_ms > 0 && dmx_data[ch] != value)
    {
        start_fade(ch, value, fade_ms);
    }
    else
    {
        stop_fade(ch);
        if (xSemaphoreTake(dmx_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            dmx_data[ch] = value;
            esp_err_t err = dmx_write(dmx_num, dmx_data, DMX_UNIVERSE_SIZE);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "DMX write failed: %s", esp_err_to_name(err));
            }
            xSemaphoreGive(dmx_mutex);
        }
        else
        {
            ESP_LOGW(TAG, "Failed to acquire mutex in set_channel");
            return CMD_ERROR_MEMORY;
        }
    }
    return CMD_SUCCESS;
}

static command_result_t set_multi_channels(int ch, int *values, int count, int fade_ms)
{
    if (!is_channel_valid(ch, count))
    {
        ESP_LOGW(TAG, "Invalid channel range: %d-%d", ch, ch + count - 1);
        return CMD_ERROR_INVALID_CHANNEL;
    }

    if (!values)
    {
        ESP_LOGW(TAG, "NULL values pointer in set_multi_channels");
        return CMD_ERROR_INVALID_VALUE;
    }

    if (fade_ms > 0)
    {
        for (int i = 0; i < count; ++i)
        {
            // Clamp values to valid range
            int clamped_value = (values[i] < 0) ? 0 : (values[i] > 255) ? 255
                                                                        : values[i];
            start_fade(ch + i, clamped_value, fade_ms);
        }
    }
    else
    {
        if (xSemaphoreTake(dmx_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            for (int i = 0; i < count; ++i)
            {
                fade_states[ch + i].active = false; // Direct access since we have mutex
                // Clamp values to valid range
                int clamped_value = (values[i] < 0) ? 0 : (values[i] > 255) ? 255
                                                                            : values[i];
                dmx_data[ch + i] = clamped_value;
            }
            esp_err_t err = dmx_write(dmx_num, dmx_data, DMX_UNIVERSE_SIZE);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "DMX write failed: %s", esp_err_to_name(err));
            }
            xSemaphoreGive(dmx_mutex);
        }
        else
        {
            ESP_LOGW(TAG, "Failed to acquire mutex in set_multi_channels");
            return CMD_ERROR_MEMORY;
        }
    }
    return CMD_SUCCESS;
}
// Command parsing with validation
typedef struct
{
    char mode;
    int channel;
    int value;
    int speed;
    bool valid;
} parsed_command_t;

static parsed_command_t parse_udp_command(const char *cmd)
{
    parsed_command_t result = {0};

    if (!cmd || strlen(cmd) < 4)
    {
        ESP_LOGW(TAG, "Command too short or NULL");
        return result;
    }

    if (strncmp(cmd, "DMX", 3) != 0)
    {
        ESP_LOGW(TAG, "Command doesn't start with DMX");
        return result;
    }

    char *copy = strdup(cmd);
    if (!copy)
    {
        ESP_LOGE(TAG, "Memory allocation failed");
        return result;
    }

    char *type = strtok(copy + 3, "#");
    char *arg1 = strtok(NULL, "#");
    char *arg2 = strtok(NULL, "#");

    if (!type || !arg1)
    {
        ESP_LOGW(TAG, "Missing required arguments in command: %s", cmd);
        free(copy);
        return result;
    }

    result.mode = type[0];
    result.channel = atoi(type + 1);
    result.value = atoi(arg1);
    result.speed = arg2 ? atoi(arg2) : 255;
    result.valid = true;

    free(copy);
    return result;
}

static command_result_t handle_udp_command(const char *cmd)
{
    static command_result_t handle_udp_command(const char *cmd)
    {
        parsed_command_t parsed = parse_udp_command(cmd);

        if (!parsed.valid)
        {
            return CMD_ERROR_INVALID_VALUE;
        }

        int ch = parsed.channel;
        int val = parsed.value;
        int fade_ms = speed_to_ms(parsed.speed);
        char mode = parsed.mode;

        // Basic channel validation for commands that need multiple channels
        if ((mode == 'L' || mode == 'W') && !is_channel_valid(ch, 2))
        {
            ESP_LOGW(TAG, "Invalid channel for %c command: %d", mode, ch);
            return CMD_ERROR_INVALID_CHANNEL;
        }
        if (mode == 'R' && !is_channel_valid(ch, 3))
        {
            ESP_LOGW(TAG, "Invalid channel for RGB command: %d", ch);
            return CMD_ERROR_INVALID_CHANNEL;
        }
        if ((mode == 'C' || mode == 'P') && !is_channel_valid(ch, 1))
        {
            ESP_LOGW(TAG, "Invalid channel for %c command: %d", mode, ch);
            return CMD_ERROR_INVALID_CHANNEL;
        }

        command_result_t result = CMD_SUCCESS;

        switch (mode)
        {
        case 'R':
        {
            // Validate RGB values are in reasonable range
            if (val < 0 || val > 999999999)
            {
                ESP_LOGW(TAG, "RGB value out of range: %d", val);
                return CMD_ERROR_INVALID_VALUE;
            }

            int r = (val % 1000);
            int g = ((val / 1000) % 1000);
            int b = ((val / 1000000) % 1000);

            // Clamp RGB values to 0-255 range
            r = (r > 255) ? 255 : r;
            g = (g > 255) ? 255 : g;
            b = (b > 255) ? 255 : b;

            int rgb[3] = {r, g, b};
            result = set_multi_channels(ch, rgb, 3, fade_ms);

            if (result == CMD_SUCCESS)
            {
                ESP_LOGI(TAG, "RGB %d: R=%d G=%d B=%d mit Fading %d ms", ch, r, g, b, fade_ms);
            }
            break;
        }
        case 'W':
        {
            int ww = (val / 1000) % 1000;
            int cw = val % 1000;

            ww = ww > 255 ? 255 : (ww < 0 ? 0 : ww);
            cw = cw > 255 ? 255 : (cw < 0 ? 0 : cw);

            int tw[2] = {ww, cw};
            result = set_multi_channels(ch, tw, 2, fade_ms);

            if (result == CMD_SUCCESS)
            {
                ESP_LOGI(TAG, "TW %d: WW=%d CW=%d with fading %d ms", ch, ww, cw, fade_ms);
            }
            break;
        }
        case 'L':
        {
            if (val < 200000000 || val > 209999999)
            {
                ESP_LOGW(TAG, "Invalid Lumitec-Command: %d", val);
                return CMD_ERROR_INVALID_VALUE;
            }

            // 1) Extract target brightness (0–100) and target CT (e.g. 2700…6500)
            int brightness = (val / 10000) % 1000;
            int color_temp = val % 10000;

            brightness = brightness < 0 ? 0 : (brightness > 100 ? 100 : brightness);

            // 2) Sort WW/CW by actual CT
            int ct_ww, ct_cw, ch_ww, ch_cw;
            get_ct_sorted(ch, &ct_ww, &ct_cw, &ch_ww, &ch_cw);

            // 3) Soll-CT auf erlaubten Bereich clampen
            if (color_temp < ct_ww)
                color_temp = ct_ww;
            if (color_temp > ct_cw)
                color_temp = ct_cw;

            // 4) Decision: which channel gets how much?
            int val_ww = 0, val_cw = 0;
            int value = brightness * 255 / 100;

            if (color_temp <= ct_ww + 100)
            {
                // fast ganz warm → nur WW an
                val_ww = value;
                val_cw = 0;
            }
            else if (color_temp >= ct_cw - 100)
            {
                // fast ganz kalt → nur CW an
                val_ww = 0;
                val_cw = value;
            }
            else
            {
                // Mischfarbe → normal berechnen
                int range = ct_cw - ct_ww;
                if (range <= 0)
                {
                    ESP_LOGW(TAG, "Invalid CT range for channel %d", ch);
                    return CMD_ERROR_CONFIG_MISSING;
                }

                long num_cw = (long)brightness * (color_temp - ct_ww) * 255;
                long num_ww = (long)brightness * (ct_cw - color_temp) * 255;
                long den = (long)range * 100;

                val_cw = (int)((num_cw + den / 2) / den);
                val_ww = (int)((num_ww + den / 2) / den);

                if (val_cw * 100 / 255 < 2)
                    val_cw = 0;
                if (val_ww * 100 / 255 < 2)
                    val_ww = 0;
            }

            // 5) Array für set_multi_channels aufbauen
            int start_ch = (ch_ww < ch_cw ? ch_ww : ch_cw);
            int values[2] = {0, 0};
            values[ch_ww - start_ch] = val_ww;
            values[ch_cw - start_ch] = val_cw;

            // 6) abschicken
            result = set_multi_channels(start_ch, values, 2, fade_ms);

            if (result == CMD_SUCCESS)
            {
                ESP_LOGI(TAG, "Lichtfarbe %dK, Helligkeit %d%% → WW=%d (CH%d), CW=%d (CH%d)",
                         color_temp, brightness, val_ww, ch_ww, val_cw, ch_cw);
            }
            break;
        }

        case 'P':
            val = (val * 255) / 100;
            // Clamp percentage to valid range
            val = (val < 0) ? 0 : (val > 255) ? 255
                                              : val;
            __attribute__((fallthrough));
        case 'C':
            result = set_channel(ch, val, fade_ms);
            if (result == CMD_SUCCESS)
            {
                ESP_LOGI(TAG, "Channel %d set to %d", ch, val);
            }
            break;
        default:
            ESP_LOGW(TAG, "Unbekannter Modus: %c", mode);
            return CMD_ERROR_INVALID_VALUE;
        }

        return result;
    }
}

void udp_server_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "UDP socket creation failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)};

    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
    {
        ESP_LOGE(TAG, "UDP socket bind failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    char rx_buffer[MAX_UDP_BUFFER_SIZE];
    struct sockaddr_in6 source_addr;
    socklen_t socklen = sizeof(source_addr);

    ESP_LOGI(TAG, "UDP server started on port %d", UDP_PORT);

    while (1)
    {
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                           (struct sockaddr *)&source_addr, &socklen);

        if (len < 0)
        {
            ESP_LOGW(TAG, "UDP recvfrom failed: errno %d", errno);
            continue;
        }

        ESP_LOGD(TAG, "UDP empfangen, Länge = %d", len);

        if (len == DMX_UNIVERSE_SIZE)
        {
            // Full DMX universe data
            if (xSemaphoreTake(dmx_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                memcpy(dmx_data, rx_buffer, DMX_UNIVERSE_SIZE);
                for (int i = 0; i < DMX_UNIVERSE_SIZE; ++i)
                    fade_states[i].active = false;

                esp_err_t err = dmx_write(dmx_num, dmx_data, DMX_UNIVERSE_SIZE);
                if (err != ESP_OK)
                {
                    ESP_LOGW(TAG, "DMX write failed: %s", esp_err_to_name(err));
                }
                xSemaphoreGive(dmx_mutex);
            }
            else
            {
                ESP_LOGW(TAG, "Failed to acquire mutex for DMX universe update");
            }
        }
        else if (len > 4 && len < MAX_UDP_BUFFER_SIZE && strncmp(rx_buffer, "DMX", 3) == 0)
        {
            rx_buffer[len] = '\0';
            ESP_LOGI(TAG, "DMX-Befehl empfangen: \"%s\"", rx_buffer);
            my_led_blink(1, 20);

            command_result_t result = handle_udp_command(rx_buffer);
            if (result != CMD_SUCCESS)
            {
                ESP_LOGW(TAG, "Command execution failed with result: %d", result);
            }
        }
        else
        {
            ESP_LOGW(TAG, "Invalid UDP packet received, length: %d", len);
        }
    }

    close(sock);
}

void app_main()
{
    esp_log_level_set("wifi", ESP_LOG_DEBUG);
    esp_log_level_set("event", ESP_LOG_DEBUG);
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize mutex for thread-safe DMX operations
    dmx_mutex = xSemaphoreCreateMutex();
    if (dmx_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create DMX mutex");
        return;
    }

    // Initialize system configuration
    esp_err_t err = system_config_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize system config: %s", esp_err_to_name(err));
        return;
    }

    const system_config_t *sys_config = system_config_get();
    system_config_print(sys_config);

    my_led_init(sys_config->hardware.debug_led_gpio);

    // Network initialization - prefer Ethernet over WiFi
    bool network_ready = false;

#ifdef CONFIG_ENABLE_ETHERNET
    if (sys_config->ethernet.enable)
    {
        ESP_LOGI(TAG, "Initializing Ethernet...");
        err = my_ethernet_init(
            sys_config->ethernet.mdc_gpio,
            sys_config->ethernet.mdio_gpio,
            sys_config->ethernet.phy_addr,
            sys_config->ethernet.phy_power_gpio,
            sys_config->ethernet.phy_rst_gpio,
            sys_config->ethernet.clock_mode,
            "udp2dmx-eth");

        if (err == ESP_OK)
        {
            err = my_ethernet_start();
            if (err == ESP_OK)
            {
                ESP_LOGI(TAG, "Ethernet started successfully");
                // Wait a bit for Ethernet to connect
                vTaskDelay(pdMS_TO_TICKS(3000));
                
                if (my_ethernet_is_connected())
                {
                    char ip_str[16];
                    if (my_ethernet_get_ip(ip_str))
                    {
                        ESP_LOGI(TAG, "Ethernet connected with IP: %s", ip_str);
                        network_ready = true;
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "Ethernet link not established; will fall back to WiFi");
                }
            }
            else
            {
                ESP_LOGW(TAG, "Failed to start Ethernet: %s", esp_err_to_name(err));
            }
        }
        else
        {
            ESP_LOGW(TAG, "Failed to initialize Ethernet: %s", esp_err_to_name(err));
        }
    }
#endif

    // Start WiFi only if Ethernet is not connected
    if (!network_ready)
    {
        ESP_LOGI(TAG, "Starting WiFi (Ethernet not connected)...");
        my_wifi_init();
        network_ready = true;
    }
    else
    {
        ESP_LOGI(TAG, "Ethernet active - WiFi skipped");
    }

    // DMX Setup
    dmx_config_t config = DMX_CONFIG_DEFAULT;
    err = dmx_driver_install(dmx_num, &config, NULL, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "DMX driver install failed: %s", esp_err_to_name(err));
        return;
    }

    err = dmx_set_pin(dmx_num, sys_config->hardware.dmx_tx_pin, sys_config->hardware.dmx_rx_pin, sys_config->hardware.dmx_en_pin);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "DMX pin setup failed: %s", esp_err_to_name(err));
        return;
    }

    memset(dmx_data, 0, sizeof(dmx_data));
    err = dmx_write(dmx_num, dmx_data, DMX_UNIVERSE_SIZE);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Initial DMX write failed: %s", esp_err_to_name(err));
    }

    // Tasks starten
    BaseType_t task_result;
    task_result = xTaskCreate(udp_server_task, "udp_server", 8192, NULL, 5, NULL);
    if (task_result != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create UDP server task");
        return;
    }

    task_result = xTaskCreate(fade_task, "fade_task", 4096, NULL, 5, NULL);
    if (task_result != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create fade task");
        return;
    }

    ESP_LOGI(TAG, "System bereit: DMX aktiv & WiFi verbunden");
    my_led_blink(2, 200);

    spiffs_init();
    config_load_from_spiffs("/spiffs/config.json");
    start_rest_server();

    TickType_t last = xTaskGetTickCount();
    while (1)
    {
        err = dmx_send(dmx_num);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "DMX send failed: %s", esp_err_to_name(err));
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(30));
    }
}
