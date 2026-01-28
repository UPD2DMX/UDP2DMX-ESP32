#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_dmx.h"

// System modules
#include "system_config.h"
#include "dmx_manager.h"
#include "udp_server.h"
#include "udp_protocol.h"

// Component modules
#include "my_wifi.h"
#include "my_led.h"
#include "my_config.h"
#include "config_handler.h"

#ifdef CONFIG_ENABLE_ETHERNET
#include "my_ethernet.h"
#endif

static const char *TAG = "main";

// System initialization functions
static esp_err_t init_system_base(void);
static esp_err_t init_system_components(void);
static esp_err_t init_dmx_system(void);
static esp_err_t init_network_services(void);
static esp_err_t start_main_loop(void);

void app_main()
{
    ESP_LOGI(TAG, "=== UDP2DMX Gateway Starting ===");

    // Initialize system base
    esp_err_t err = init_system_base();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize system base: %s", esp_err_to_name(err));
        return;
    }

    // Initialize system components
    err = init_system_components();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize system components: %s", esp_err_to_name(err));
        return;
    }

    // Initialize network services first (like in working version)
    err = init_network_services();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize network services: %s", esp_err_to_name(err));
        return;
    }

    // Initialize DMX system AFTER WiFi (like in working version)
    err = init_dmx_system();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize DMX system: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "=== System initialization complete ===");
    
    // Start main loop
    start_main_loop();
}

static esp_err_t init_system_base(void)
{
    ESP_LOGI(TAG, "Initializing system base...");

    // Set log levels
    esp_log_level_set("wifi", ESP_LOG_DEBUG);
    esp_log_level_set("event", ESP_LOG_DEBUG);

    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Initialize network interface
    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Network interface init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create default event loop
    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Event loop creation failed: %s", esp_err_to_name(err));
        return err;
    }

    // Initialize system configuration
    err = system_config_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "System config init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Print system configuration
    system_config_print(system_config_get());

    return ESP_OK;
}

static esp_err_t init_system_components(void)
{
    ESP_LOGI(TAG, "Initializing system components...");
    
    const system_config_t *config = system_config_get();

    // Initialize LED
    my_led_init(config->hardware.debug_led_gpio);

    // Initialize SPIFFS first to load hostname later
    spiffs_init();

    // Try Ethernet first (if enabled)
    bool network_ready = false;
    
#ifdef CONFIG_ENABLE_ETHERNET
    if (config->ethernet.enable)
    {
        ESP_LOGI(TAG, "Initializing Ethernet...");
        esp_err_t err = my_ethernet_init(
            config->ethernet.mdc_gpio,
            config->ethernet.mdio_gpio,
            config->ethernet.phy_addr,
            config->ethernet.phy_power_gpio,
            config->ethernet.phy_rst_gpio,
            config->ethernet.clock_mode,
            "udp2dmx");  // Use default, will be updated after config load

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

    // Initialize WiFi only if Ethernet is not connected
    if (!network_ready)
    {
        ESP_LOGI(TAG, "Starting WiFi (Ethernet not available or not connected)...");
        my_wifi_init();
    }
    else
    {
        ESP_LOGI(TAG, "Network ready via Ethernet, skipping WiFi initialization");
    }

    // Load configuration from SPIFFS (hostname will be set via my_wifi_set_hostname)
    config_load_from_spiffs("/spiffs/config.json");

    return ESP_OK;
}

static esp_err_t init_dmx_system(void)
{
    ESP_LOGI(TAG, "Initializing DMX system...");
    
    // Wait a bit for system stability (like in working version)
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "System settled, proceeding with DMX initialization");
    
    const system_config_t *config = system_config_get();

    // Initialize DMX manager
    esp_err_t err = dmx_manager_init(
        config->hardware.dmx_tx_pin,
        config->hardware.dmx_rx_pin,
        config->hardware.dmx_en_pin
    );
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DMX manager initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    // Initialize UDP protocol
    err = udp_protocol_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UDP protocol initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t init_network_services(void)
{
    ESP_LOGI(TAG, "Initializing network services...");
    
    const system_config_t *config = system_config_get();

    // Initialize UDP server
    esp_err_t err = udp_server_init(config->network.udp_port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UDP server initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    // Start UDP server
    err = udp_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start UDP server: %s", esp_err_to_name(err));
        return err;
    }

    // Start REST server for configuration
    start_rest_server();

    // Signal successful startup
    my_led_blink(2, 200);

    return ESP_OK;
}

static esp_err_t start_main_loop(void)
{
    ESP_LOGI(TAG, "Starting main loop...");

    TickType_t last_wake_time = xTaskGetTickCount();
    
    while (1) {
        // Continuous DMX sending - exact timing from working code
        dmx_send(DMX_NUM_1);
        
        // Use exact 30ms timing from working version
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(30));
    }

    return ESP_OK;
}
