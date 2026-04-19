#include "udp_server.h"
#include "udp_protocol.h"
#include "dmx_manager.h"
#include "my_led.h"

#include <string.h>
#include <errno.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "udp_server";

// Server state
static bool server_initialized = false;
static bool server_running = false;
static int server_socket = -1;
static uint16_t server_port = UDP_DEFAULT_PORT;
static TaskHandle_t server_task_handle = NULL;

// Statistics
static udp_server_stats_t server_stats = {0};

// Private function declarations
static void udp_server_task(void *arg);
static esp_err_t handle_dmx_universe_data(const uint8_t *data, size_t len);
static esp_err_t handle_dmx_command(const char *cmd);
static bool dmx_universe_matches(const uint8_t *data);

// Initialize UDP server
esp_err_t udp_server_init(uint16_t port)
{
    if (server_initialized)
    {
        ESP_LOGW(TAG, "UDP server already initialized");
        return ESP_OK;
    }

    server_port = port;
    memset(&server_stats, 0, sizeof(server_stats));
    server_initialized = true;

    ESP_LOGI(TAG, "UDP server initialized on port %d", port);
    return ESP_OK;
}

void udp_server_deinit(void)
{
    if (!server_initialized)
    {
        return;
    }

    udp_server_stop();
    server_initialized = false;
    ESP_LOGI(TAG, "UDP server deinitialized");
}

bool udp_server_is_running(void)
{
    return server_running;
}

// Start UDP server
esp_err_t udp_server_start(void)
{
    if (!server_initialized)
    {
        ESP_LOGE(TAG, "UDP server not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (server_running)
    {
        ESP_LOGW(TAG, "UDP server already running");
        return ESP_OK;
    }

    // Set running flag BEFORE creating the task.
    // Otherwise the task can reach the receive loop while server_running is still false
    // and exit immediately (race condition).
    server_running = true;

    // Create server task
    BaseType_t task_result = xTaskCreate(
        udp_server_task,
        "udp_server",
        8192,
        NULL,
        5,
        &server_task_handle);

    if (task_result != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create UDP server task");
        server_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UDP server started");
    return ESP_OK;
}

// Stop UDP server
esp_err_t udp_server_stop(void)
{
    if (!server_running)
    {
        return ESP_OK;
    }

    server_running = false;

    // Close socket to interrupt blocking recvfrom
    if (server_socket >= 0)
    {
        close(server_socket);
        server_socket = -1;
    }

    // Delete task
    if (server_task_handle != NULL)
    {
        vTaskDelete(server_task_handle);
        server_task_handle = NULL;
    }

    ESP_LOGI(TAG, "UDP server stopped");
    return ESP_OK;
}

// Restart UDP server
esp_err_t udp_server_restart(void)
{
    esp_err_t err = udp_server_stop();
    if (err != ESP_OK)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(100)); // Brief delay
    return udp_server_start();
}

// Get server statistics
udp_server_stats_t udp_server_get_stats(void)
{
    return server_stats;
}

// Reset server statistics
void udp_server_reset_stats(void)
{
    memset(&server_stats, 0, sizeof(server_stats));
    ESP_LOGI(TAG, "Server statistics reset");
}

// Private functions

// Main server task
static void udp_server_task(void *arg)
{
    ESP_LOGI(TAG, "UDP server task started on port %d", server_port);

    // Create UDP socket
    server_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (server_socket < 0)
    {
        ESP_LOGE(TAG, "UDP socket creation failed: errno %d", errno);
        server_running = false;
        vTaskDelete(NULL);
        return;
    }

    // Bind socket
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(server_port),
        .sin_addr.s_addr = htonl(INADDR_ANY)};

    if (bind(server_socket, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0)
    {
        ESP_LOGE(TAG, "UDP socket bind failed: errno %d", errno);
        close(server_socket);
        server_socket = -1;
        server_running = false;
        vTaskDelete(NULL);
        return;
    }

    char rx_buffer[UDP_BUFFER_SIZE];
    struct sockaddr_in6 source_addr;
    socklen_t socklen = sizeof(source_addr);

    ESP_LOGI(TAG, "UDP server listening on port %d", server_port);

    while (server_running)
    {
        int len = recvfrom(server_socket, rx_buffer, sizeof(rx_buffer) - 1, 0,
                           (struct sockaddr *)&source_addr, &socklen);

        server_stats.packets_received++;

        if (len < 0)
        {
            if (server_running)
            { // Only log if we're supposed to be running
                ESP_LOGW(TAG, "UDP recvfrom failed: errno %d", errno);
            }
            continue;
        }

        ESP_LOGD(TAG, "UDP packet received, length = %d", len);

        if (len == DMX_UNIVERSE_SIZE)
        {
            // Full DMX universe data
            esp_err_t err = handle_dmx_universe_data((uint8_t *)rx_buffer, len);
            if (err == ESP_OK)
            {
                server_stats.packets_processed++;
            }
            else
            {
                server_stats.packets_invalid++;
            }
        }
        else if (len > 4 && len < UDP_BUFFER_SIZE && strncmp(rx_buffer, "DMX", 3) == 0)
        {
            // DMX command
            rx_buffer[len] = '\0';
            ESP_LOGD(TAG, "DMX command received: \"%s\"", rx_buffer);

            // Visual feedback
            my_led_blink(1, 20);

            esp_err_t err = handle_dmx_command(rx_buffer);
            if (err == ESP_OK)
            {
                server_stats.packets_processed++;
                server_stats.commands_executed++;
            }
            else
            {
                server_stats.packets_invalid++;
                server_stats.command_errors++;
            }
        }
        else
        {
            ESP_LOGW(TAG, "Invalid UDP packet received, length: %d", len);
            server_stats.packets_invalid++;
        }
    }

    // Cleanup
    if (server_socket >= 0)
    {
        close(server_socket);
        server_socket = -1;
    }

    ESP_LOGI(TAG, "UDP server task ended");
    server_running = false;
    vTaskDelete(NULL);
}

// Handle full DMX universe data
static esp_err_t handle_dmx_universe_data(const uint8_t *data, size_t len)
{
    if (!data || len != DMX_UNIVERSE_SIZE)
    {
        ESP_LOGW(TAG, "Invalid DMX universe data");
        return ESP_ERR_INVALID_ARG;
    }

    if (dmx_universe_matches(data))
    {
        ESP_LOGD(TAG, "DMX universe unchanged; skipping update");
        return ESP_OK;
    }

    // Stop all current fades and update all channels
    dmx_stop_all_fades();

    dmx_command_result_t result = dmx_set_multi_channels(1, data, DMX_UNIVERSE_SIZE, 0);

    if (result == DMX_CMD_SUCCESS)
    {
        ESP_LOGD(TAG, "DMX universe updated successfully");
        return ESP_OK;
    }
    else
    {
        ESP_LOGW(TAG, "Failed to update DMX universe: %d", result);
        return ESP_FAIL;
    }
}

static bool dmx_universe_matches(const uint8_t *data)
{
    if (!data)
    {
        return false;
    }

    for (int channel = 1; channel <= DMX_UNIVERSE_SIZE; ++channel)
    {
        if (dmx_get_channel_value(channel) != data[channel - 1])
        {
            return false;
        }
    }

    return true;
}

// Handle DMX command
static esp_err_t handle_dmx_command(const char *cmd)
{
    if (!cmd)
    {
        ESP_LOGW(TAG, "NULL command string");
        return ESP_ERR_INVALID_ARG;
    }

    dmx_command_result_t result = udp_handle_raw_command(cmd);

    if (result == DMX_CMD_SUCCESS)
    {
        ESP_LOGD(TAG, "Command executed successfully: %s", cmd);
        return ESP_OK;
    }
    else
    {
        ESP_LOGW(TAG, "Command execution failed: %s (result: %d)", cmd, result);
        return ESP_FAIL;
    }
}
