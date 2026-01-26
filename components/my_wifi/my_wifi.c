#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "mdns.h"

#include "my_wifi.h"
#include "my_led.h"
#include "sdkconfig.h"

#define WIFI_CONFIG_NAMESPACE "wifi_config"
#define WIFI_SWITCH_BUTTON_GPIO CONFIG_WIFI_SWITCH_BUTTON_GPIO

static const char *TAG = "my_wifi";
static bool is_connecting = false;
static char current_hostname[32] = "udp2dmx";
static TaskHandle_t reconnect_task_handle = NULL;
static bool wifi_connected = false;

// ==================== NVS Functions ====================

bool my_wifi_nvs_save_config(const wifi_config_nvs_t *config)
{
    if (!config || strlen(config->ssid) == 0)
    {
        ESP_LOGE(TAG, "Invalid config for saving");
        return false;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WIFI_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }

    // Save all config fields as binary blob
    err = nvs_set_blob(nvs_handle, "primary_wifi", (const void *)config, sizeof(wifi_config_nvs_t));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "WiFi config saved: SSID=%s, DHCP=%s", config->ssid, config->use_dhcp ? "yes" : "no");
        return true;
    }
    else
    {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
        return false;
    }
}

bool my_wifi_nvs_load_config(wifi_config_nvs_t *config)
{
    if (!config)
    {
        ESP_LOGE(TAG, "Config pointer is NULL");
        return false;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WIFI_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "NVS open failed: %s (first boot?)", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    size_t required_size = sizeof(wifi_config_nvs_t);
    err = nvs_get_blob(nvs_handle, "primary_wifi", (void *)config, &required_size);
    nvs_close(nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "No WiFi config found in NVS");
        return false;
    }
    else if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS read failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "WiFi config loaded: SSID=%s, DHCP=%s", config->ssid, config->use_dhcp ? "yes" : "no");
    return true;
}

bool my_wifi_nvs_delete_config(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WIFI_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS open failed");
        return false;
    }

    err = nvs_erase_key(nvs_handle, "primary_wifi");
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "No WiFi config to delete");
        nvs_close(nvs_handle);
        return false;
    }
    else if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "WiFi config deleted");
    return true;
}

// ==================== WiFi Connection Functions ====================

void my_wifi_set_hostname(const char *new_hostname)
{
    if (!new_hostname || strlen(new_hostname) >= sizeof(current_hostname))
    {
        ESP_LOGW(TAG, "Hostname invalid or too long: %s", new_hostname);
        return;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif)
    {
        ESP_LOGE(TAG, "esp_netif not found");
        return;
    }

    const char *old_hostname = NULL;
    if (esp_netif_get_hostname(netif, &old_hostname) != ESP_OK)
    {
        ESP_LOGW(TAG, "Could not read old hostname");
        old_hostname = NULL;
    }

    // Only update if hostname changes
    if (old_hostname && strcmp(old_hostname, new_hostname) == 0)
    {
        ESP_LOGI(TAG, "Hostname is already: %s", old_hostname);
        return;
    }

    // Store and set hostname
    strncpy(current_hostname, new_hostname, sizeof(current_hostname));
    current_hostname[sizeof(current_hostname) - 1] = '\0';

    esp_netif_set_hostname(netif, current_hostname);
    ESP_LOGI(TAG, "Hostname changed: %s", current_hostname);

    // Update mDNS
    mdns_free();
    mdns_init();
    mdns_hostname_set(current_hostname);
    ESP_LOGI(TAG, "mDNS hostname updated to: %s", current_hostname);
}

void start_mdns_service(void)
{
    mdns_init();
    mdns_hostname_set(current_hostname);
    mdns_instance_name_set("DMX Controller");
    ESP_LOGI(TAG, "mDNS hostname set: %s", current_hostname);
}

bool my_wifi_is_connected(void)
{
    return wifi_connected;
}

static void connect_to_wifi_config(const wifi_config_nvs_t *config)
{
    if (is_connecting)
    {
        ESP_LOGW(TAG, "Connection attempt already in progress");
        return;
    }

    if (!config || strlen(config->ssid) == 0)
    {
        ESP_LOGE(TAG, "Invalid WiFi config");
        return;
    }

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, config->ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, config->password, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Configure static IP if needed
    if (!config->use_dhcp)
    {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif)
        {
            esp_netif_dhcpc_stop(netif);

            esp_netif_ip_info_t ip_info = {
                .ip.addr = config->static_ip,
                .netmask.addr = config->subnet_mask,
                .gw.addr = config->gateway,
            };
            esp_netif_set_ip_info(netif, &ip_info);

            // Set DNS server
            esp_netif_dns_info_t dns_info = {
                .ip.u_addr.ip4.addr = config->dns,
            };
            esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info);

            ESP_LOGI(TAG, "Static IP configured");
        }
    }

    is_connecting = true;
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "Connecting to SSID: %s", config->ssid);
}

bool my_wifi_connect_to_primary(const wifi_config_nvs_t *config, uint32_t timeout_ms)
{
    // Set WiFi to STA mode
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    connect_to_wifi_config(config);

    // Wait for connection with timeout
    uint32_t elapsed = 0;
    const uint32_t check_interval = 500; // Check every 500ms

    while (elapsed < timeout_ms)
    {
        if (wifi_connected)
        {
            ESP_LOGI(TAG, "Successfully connected to primary WiFi");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(check_interval));
        elapsed += check_interval;
    }

    ESP_LOGW(TAG, "Timeout connecting to primary WiFi after %" PRIu32 " ms", timeout_ms);
    return false;
}

void my_wifi_start_config_mode(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Starting Config WiFi AP: %s", ssid);

    // Stop and deinit existing WiFi
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Deinit WiFi completely to reset state
    esp_wifi_deinit();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Re-initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Create AP interface
    esp_netif_create_default_wifi_ap();

    // Set AP mode BEFORE starting
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    // Configure AP
    wifi_config_t ap_config = {
        .ap = {
            .max_connection = 4,
            .authmode = (password && strlen(password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        },
    };

    strncpy((char *)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid) - 1);
    if (password && strlen(password) > 0)
    {
        strncpy((char *)ap_config.ap.password, password, sizeof(ap_config.ap.password) - 1);
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Config WiFi AP started. Connect to: %s", ssid);
}

// ==================== Event Handlers ====================

const char *reason_str(wifi_err_reason_t reason)
{
    switch (reason)
    {
    case WIFI_REASON_AUTH_EXPIRE:
        return "Authentication expired";
    case WIFI_REASON_AUTH_FAIL:
        return "Authentication failed";
    case WIFI_REASON_NO_AP_FOUND:
        return "AP not found";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "Handshake timeout";
    case WIFI_REASON_ASSOC_FAIL:
        return "Association failed";
    default:
        return "Unknown reason";
    }
}

static void on_wifi_event(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnected (reason %d: %s)", disconn->reason, reason_str(disconn->reason));
        my_led_set_wifi_status(false);
        is_connecting = false;
        wifi_connected = false;
        xTaskNotifyGive(reconnect_task_handle);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected - IP: " IPSTR, IP2STR(&event->ip_info.ip));
        is_connecting = false;
        wifi_connected = true;
        my_led_set_wifi_status(true);
        start_mdns_service();
    }
}

void reconnect_task(void *param)
{
    while (1)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!is_connecting && !wifi_connected)
        {
            // Try to reconnect with stored config
            wifi_config_nvs_t config;
            if (my_wifi_nvs_load_config(&config))
            {
                connect_to_wifi_config(&config);
            }
        }
    }
}

// ==================== Initialization ====================

void my_wifi_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Error creating event loop: %s", esp_err_to_name(err));
        return;
    }

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(err));
        return;
    }

    // Register event handlers
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL);

    // Try to load primary WiFi config
    wifi_config_nvs_t primary_wifi = {0};
    bool has_config = my_wifi_nvs_load_config(&primary_wifi);

    if (has_config)
    {
        // Set STA mode
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

        // Create STA interface for primary WiFi
        esp_netif_create_default_wifi_sta();
        ESP_ERROR_CHECK(esp_wifi_start());

        // Try connecting with 10 second timeout
        if (my_wifi_connect_to_primary(&primary_wifi, 10000))
        {
            ESP_LOGI(TAG, "Connected to primary WiFi");
            xTaskCreate(reconnect_task, "wifi_reconnect_task", 4096, NULL, 5, &reconnect_task_handle);
            return;
        }
        else
        {
            ESP_LOGW(TAG, "Failed to connect to primary WiFi, starting config mode");
            esp_wifi_stop();
        }
    }
    else
    {
        ESP_LOGI(TAG, "No primary WiFi config found, starting config mode");
    }

    // Start Config WiFi (AP Mode)
    my_wifi_start_config_mode(CONFIG_WIFI_BACKUP_SSID, CONFIG_WIFI_BACKUP_PASSWORD);
}
