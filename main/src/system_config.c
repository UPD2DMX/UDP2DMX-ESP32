#include "system_config.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "system_config";
static const char *NVS_NAMESPACE = "system_cfg";

// Default configuration - loaded from Menuconfig (sdkconfig.h)
static system_config_t default_config = {
    .hardware = {
        .dmx_tx_pin = CONFIG_DMX_TX_GPIO,
        .dmx_rx_pin = CONFIG_DMX_RX_GPIO,
        .dmx_en_pin = CONFIG_DMX_RTS_GPIO,
        .debug_led_gpio = CONFIG_DEBUG_LED_GPIO},
#ifdef CONFIG_ENABLE_ETHERNET
    .ethernet = {
        .enable = true,
        .mdc_gpio = CONFIG_ETH_MDC_GPIO,
        .mdio_gpio = CONFIG_ETH_MDIO_GPIO,
        .phy_addr = CONFIG_ETH_PHY_ADDR,
        .phy_power_gpio = CONFIG_ETH_PHY_POWER_GPIO,
        .phy_rst_gpio = CONFIG_ETH_PHY_RST_GPIO,
#if defined(CONFIG_ETH_CLOCK_GPIO0_IN)
        .clock_mode = 0,
#elif defined(CONFIG_ETH_CLOCK_GPIO0_OUT)
        .clock_mode = 1,
#elif defined(CONFIG_ETH_CLOCK_GPIO16_OUT)
        .clock_mode = 2,
#elif defined(CONFIG_ETH_CLOCK_GPIO17_OUT)
        .clock_mode = 3,
#endif
    },
#else
    .ethernet = {
        .enable = false,
        .mdc_gpio = -1,
        .mdio_gpio = -1,
        .phy_addr = 0,
        .phy_power_gpio = -1,
        .phy_rst_gpio = -1,
        .clock_mode = 0,
    },
#endif
    .network = {.udp_port = 6454, .max_udp_buffer_size = 1024},
    .dmx = {.universe_size = 512, .fade_interval_ms = CONFIG_DMX_FADE_INTERVAL_MS},
    .system = {.enable_debug_logging = false, .watchdog_timeout_ms = 30000}};

static system_config_t current_config;
static bool config_initialized = false;

esp_err_t system_config_init(void)
{
    if (config_initialized)
    {
        ESP_LOGW(TAG, "System config already initialized");
        return ESP_OK;
    }

    // Load default configuration
    memcpy(&current_config, &default_config, sizeof(system_config_t));

    // Try to load from NVS
    esp_err_t err = system_config_load_from_nvs();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to load config from NVS, using defaults");
    }

    config_initialized = true;
    ESP_LOGI(TAG, "System configuration initialized");

    return ESP_OK;
}

const system_config_t *system_config_get(void)
{
    if (!config_initialized)
    {
        ESP_LOGW(TAG, "System config not initialized, returning defaults");
        return &default_config;
    }

    return &current_config;
}

esp_err_t system_config_load_from_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    size_t required_size = sizeof(system_config_t);
    err = nvs_get_blob(nvs_handle, "config", &current_config, &required_size);
    nvs_close(nvs_handle);

    if (err == ESP_OK)
    {
        if (!system_config_validate(&current_config))
        {
            ESP_LOGW(TAG, "Loaded config is invalid, using defaults");
            memcpy(&current_config, &default_config, sizeof(system_config_t));
            return ESP_ERR_INVALID_CRC;
        }
        ESP_LOGI(TAG, "Configuration loaded from NVS");
    }
    else
    {
        ESP_LOGW(TAG, "Failed to load config from NVS: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t system_config_save_to_nvs(void)
{
    if (!system_config_validate(&current_config))
    {
        ESP_LOGE(TAG, "Cannot save invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(nvs_handle, "config", &current_config, sizeof(system_config_t));
    if (err == ESP_OK)
    {
        err = nvs_commit(nvs_handle);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Configuration saved to NVS");
        }
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t system_config_load_defaults(void)
{
    memcpy(&current_config, &default_config, sizeof(system_config_t));
    ESP_LOGI(TAG, "Default configuration loaded");
    return ESP_OK;
}

bool system_config_validate(const system_config_t *config)
{
    if (!config)
    {
        return false;
    }

    // Validate hardware pins
    if (config->hardware.dmx_tx_pin < 0 || config->hardware.dmx_tx_pin > 39 ||
        config->hardware.dmx_rx_pin < 0 || config->hardware.dmx_rx_pin > 39 ||
        config->hardware.dmx_en_pin < 0 || config->hardware.dmx_en_pin > 39 ||
        config->hardware.debug_led_gpio < 0 || config->hardware.debug_led_gpio > 39)
    {
        ESP_LOGW(TAG, "Invalid GPIO pin configuration");
        return false;
    }

    // Validate Ethernet pins if enabled
    if (config->ethernet.enable)
    {
        if (config->ethernet.mdc_gpio < 0 || config->ethernet.mdc_gpio > 39 ||
            config->ethernet.mdio_gpio < 0 || config->ethernet.mdio_gpio > 39)
        {
            ESP_LOGW(TAG, "Invalid Ethernet pin configuration");
            return false;
        }
        if (config->ethernet.phy_addr < 0 || config->ethernet.phy_addr > 31)
        {
            ESP_LOGW(TAG, "Invalid PHY address: %d", config->ethernet.phy_addr);
            return false;
        }
    }

    // Validate network settings
    if (config->network.udp_port == 0)
    {
        ESP_LOGW(TAG, "Invalid UDP port: %d", config->network.udp_port);
        return false;
    }

    if (config->network.max_udp_buffer_size < 64 || config->network.max_udp_buffer_size > 8192)
    {
        ESP_LOGW(TAG, "Invalid UDP buffer size: %d", config->network.max_udp_buffer_size);
        return false;
    }

    // Validate DMX settings
    if (config->dmx.universe_size < 1 || config->dmx.universe_size > 512)
    {
        ESP_LOGW(TAG, "Invalid DMX universe size: %d", config->dmx.universe_size);
        return false;
    }

    if (config->dmx.fade_interval_ms < 1 || config->dmx.fade_interval_ms > 1000)
    {
        ESP_LOGW(TAG, "Invalid fade interval: %d", config->dmx.fade_interval_ms);
        return false;
    }

    // Validate system settings
    if (config->system.watchdog_timeout_ms < 1000 || config->system.watchdog_timeout_ms > 300000)
    {
        ESP_LOGW(TAG, "Invalid watchdog timeout: %d", config->system.watchdog_timeout_ms);
        return false;
    }

    return true;
}

void system_config_print(const system_config_t *config)
{
    if (!config)
    {
        ESP_LOGE(TAG, "NULL configuration pointer");
        return;
    }

    ESP_LOGI(TAG, "=== System Configuration ===");
    ESP_LOGI(TAG, "Hardware:");
    ESP_LOGI(TAG, "  DMX TX Pin: %d", config->hardware.dmx_tx_pin);
    ESP_LOGI(TAG, "  DMX RX Pin: %d", config->hardware.dmx_rx_pin);
    ESP_LOGI(TAG, "  DMX EN Pin: %d", config->hardware.dmx_en_pin);
    ESP_LOGI(TAG, "  Debug LED GPIO: %d", config->hardware.debug_led_gpio);

    if (config->ethernet.enable)
    {
        ESP_LOGI(TAG, "Ethernet:");
        ESP_LOGI(TAG, "  Enabled: Yes");
        ESP_LOGI(TAG, "  MDC GPIO: %d", config->ethernet.mdc_gpio);
        ESP_LOGI(TAG, "  MDIO GPIO: %d", config->ethernet.mdio_gpio);
        ESP_LOGI(TAG, "  PHY Address: %d", config->ethernet.phy_addr);
        ESP_LOGI(TAG, "  PHY Power GPIO: %d", config->ethernet.phy_power_gpio);
        ESP_LOGI(TAG, "  PHY Reset GPIO: %d", config->ethernet.phy_rst_gpio);
        ESP_LOGI(TAG, "  Clock Mode: %d", config->ethernet.clock_mode);
    }
    else
    {
        ESP_LOGI(TAG, "Ethernet: Disabled");
    }

    ESP_LOGI(TAG, "Network:");
    ESP_LOGI(TAG, "  UDP Port: %d", config->network.udp_port);
    ESP_LOGI(TAG, "  Max UDP Buffer: %d", config->network.max_udp_buffer_size);

    ESP_LOGI(TAG, "DMX:");
    ESP_LOGI(TAG, "  Universe Size: %d", config->dmx.universe_size);
    ESP_LOGI(TAG, "  Fade Interval: %d ms", config->dmx.fade_interval_ms);

    ESP_LOGI(TAG, "System:");
    ESP_LOGI(TAG, "  Debug Logging: %s", config->system.enable_debug_logging ? "Yes" : "No");
    ESP_LOGI(TAG, "  Watchdog Timeout: %d ms", config->system.watchdog_timeout_ms);
    ESP_LOGI(TAG, "=== End Configuration ===");
}
