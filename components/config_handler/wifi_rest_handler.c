#include "config_handler.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "my_wifi.h"
#include "my_led.h"
#include "my_ethernet.h"

static const char *TAG = "wifi_rest";

// ==================== Forward Declarations ====================

// ==================== Helper Functions ====================

/**
 * @brief Get WiFi connection status
 */
static bool wifi_is_connected(void)
{
    return my_wifi_is_connected();
}

/**
 * @brief Get current IP address (WiFi or Ethernet)
 */
static void wifi_get_ip_address(char *ip_str, size_t len)
{
    esp_netif_t *netif = NULL;
    
    // Try Ethernet first
    if (my_ethernet_is_connected())
    {
        netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    }
    
    // Fall back to WiFi if Ethernet not connected
    if (!netif)
    {
        netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    }
    
    if (!netif)
    {
        strncpy(ip_str, "0.0.0.0", len);
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK)
    {
        strncpy(ip_str, "0.0.0.0", len);
        return;
    }

    snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
}

/**
 * @brief Get WiFi RSSI (signal strength)
 */
static int8_t wifi_get_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
        return ap_info.rssi;
    }
    return 0;
}

/**
 * @brief Get WiFi SSID
 */
static void wifi_get_ssid(char *ssid, size_t len)
{
    wifi_config_t wifi_config;
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_config) == ESP_OK)
    {
        strncpy(ssid, (const char *)wifi_config.sta.ssid, len - 1);
        ssid[len - 1] = '\0';
    }
}

/**
 * @brief Get connection type (LAN or WiFi)
 */
static const char *get_connection_type_string(void)
{
    if (my_ethernet_is_connected())
    {
        return "LAN";
    }
    else if (wifi_is_connected())
    {
        return "WiFi";
    }
    else
    {
        return "None";
    }
}

// ==================== REST Handlers ====================

/**
 * @brief GET /api/wifi - Returns current WiFi configuration and status
 */
esp_err_t wifi_get_config_handler(httpd_req_t *req)
{
    wifi_config_nvs_t wifi_config = {0};
    char ip_str[16] = "0.0.0.0";
    char ssid[32] = "";

    // Load config from NVS
    my_wifi_nvs_load_config(&wifi_config);
    wifi_get_ip_address(ip_str, sizeof(ip_str));
    wifi_get_ssid(ssid, sizeof(ssid));

    // Build JSON response
    cJSON *root = cJSON_CreateObject();
    if (!root)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *primary_wifi = cJSON_CreateObject();
    cJSON_AddStringToObject(primary_wifi, "ssid", wifi_config.ssid);
    cJSON_AddBoolToObject(primary_wifi, "use_dhcp", wifi_config.use_dhcp);
    
    // Convert static IP from uint32_t to string
    char ip_buf[16];
    snprintf(ip_buf, sizeof(ip_buf), "%lu.%lu.%lu.%lu",
        (wifi_config.static_ip >> 0) & 0xFF,
        (wifi_config.static_ip >> 8) & 0xFF,
        (wifi_config.static_ip >> 16) & 0xFF,
        (wifi_config.static_ip >> 24) & 0xFF);
    cJSON_AddStringToObject(primary_wifi, "static_ip", ip_buf);

    char gateway_buf[16];
    snprintf(gateway_buf, sizeof(gateway_buf), "%lu.%lu.%lu.%lu",
        (wifi_config.gateway >> 0) & 0xFF,
        (wifi_config.gateway >> 8) & 0xFF,
        (wifi_config.gateway >> 16) & 0xFF,
        (wifi_config.gateway >> 24) & 0xFF);
    cJSON_AddStringToObject(primary_wifi, "gateway", gateway_buf);

    char dns_buf[16];
    snprintf(dns_buf, sizeof(dns_buf), "%lu.%lu.%lu.%lu",
        (wifi_config.dns >> 0) & 0xFF,
        (wifi_config.dns >> 8) & 0xFF,
        (wifi_config.dns >> 16) & 0xFF,
        (wifi_config.dns >> 24) & 0xFF);
    cJSON_AddStringToObject(primary_wifi, "dns", dns_buf);

    cJSON_AddItemToObject(root, "primary_wifi", primary_wifi);
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_is_connected());
    cJSON_AddStringToObject(root, "connection_type", get_connection_type_string());
    cJSON_AddBoolToObject(root, "lan_connected", my_ethernet_is_connected());
    cJSON_AddStringToObject(root, "current_ip", ip_str);
    cJSON_AddStringToObject(root, "current_ssid", ssid);
    cJSON_AddNumberToObject(root, "rssi", wifi_get_rssi());

    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, (const char *)json_str, HTTPD_RESP_USE_STRLEN);

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief POST /api/wifi - Save new WiFi configuration
 */
esp_err_t wifi_post_config_handler(httpd_req_t *req)
{
    char buffer[1024];
    int total_len = req->content_len;

    if (total_len <= 0 || total_len >= sizeof(buffer))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buffer, total_len);
    if (ret <= 0)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buffer[ret] = '\0';

    // Parse JSON
    cJSON *root = cJSON_Parse(buffer);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Extract fields
    cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
    cJSON *password_item = cJSON_GetObjectItem(root, "password");
    cJSON *use_dhcp_item = cJSON_GetObjectItem(root, "use_dhcp");
    cJSON *static_ip_item = cJSON_GetObjectItem(root, "static_ip");
    cJSON *gateway_item = cJSON_GetObjectItem(root, "gateway");
    cJSON *dns_item = cJSON_GetObjectItem(root, "dns");

    // Validate required fields
    if (!ssid_item || !ssid_item->valuestring || strlen(ssid_item->valuestring) == 0)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    if (!password_item || !password_item->valuestring)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password required");
        return ESP_FAIL;
    }

    // Build WiFi config structure
    wifi_config_nvs_t wifi_config = {0};
    strncpy(wifi_config.ssid, ssid_item->valuestring, sizeof(wifi_config.ssid) - 1);
    strncpy(wifi_config.password, password_item->valuestring, sizeof(wifi_config.password) - 1);
    wifi_config.use_dhcp = use_dhcp_item ? use_dhcp_item->valueint : true;
    wifi_config.auth_mode = WIFI_AUTH_WPA2_PSK;

    // Parse static IP settings if not DHCP
    if (!wifi_config.use_dhcp)
    {
        if (static_ip_item && static_ip_item->valuestring)
        {
            unsigned int ip[4];
            sscanf(static_ip_item->valuestring, "%u.%u.%u.%u", &ip[3], &ip[2], &ip[1], &ip[0]);
            wifi_config.static_ip = (ip[3] << 24) | (ip[2] << 16) | (ip[1] << 8) | ip[0];
        }

        if (gateway_item && gateway_item->valuestring)
        {
            unsigned int ip[4];
            sscanf(gateway_item->valuestring, "%u.%u.%u.%u", &ip[3], &ip[2], &ip[1], &ip[0]);
            wifi_config.gateway = (ip[3] << 24) | (ip[2] << 16) | (ip[1] << 8) | ip[0];
        }

        if (dns_item && dns_item->valuestring)
        {
            unsigned int ip[4];
            sscanf(dns_item->valuestring, "%u.%u.%u.%u", &ip[3], &ip[2], &ip[1], &ip[0]);
            wifi_config.dns = (ip[3] << 24) | (ip[2] << 16) | (ip[1] << 8) | ip[0];
        }
    }

    cJSON_Delete(root);

    // Save to NVS
    if (!my_wifi_nvs_save_config(&wifi_config))
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save WiFi config");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WiFi configuration saved: SSID=%s, DHCP=%s", wifi_config.ssid, 
             wifi_config.use_dhcp ? "yes" : "no");

    // Send response
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "WiFi configuration saved. Restarting...");
    
    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, (const char *)json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    cJSON_Delete(response);

    // Restart to apply new WiFi config
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

/**
 * @brief GET /api/system - Returns system information
 */
esp_err_t system_info_handler(httpd_req_t *req)
{
    wifi_config_t wifi_config;
    char ip_str[16] = "0.0.0.0";
    char mac_str[18] = "";
    
    esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
    wifi_get_ip_address(ip_str, sizeof(ip_str));

    // Get MAC address
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cJSON *root = cJSON_CreateObject();
    if (!root)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    cJSON_AddStringToObject(root, "hostname", "udp2dmx");
    cJSON_AddNumberToObject(root, "uptime_seconds", esp_timer_get_time() / 1000000);
    cJSON_AddStringToObject(root, "app_version", app_desc ? app_desc->version : "unknown");
    cJSON_AddStringToObject(root, "idf_version", app_desc ? app_desc->idf_ver : esp_get_idf_version());
    cJSON_AddStringToObject(root, "app_build_time", app_desc ? app_desc->time : "");
    cJSON_AddStringToObject(root, "app_build_date", app_desc ? app_desc->date : "");
    cJSON_AddStringToObject(root, "running_partition", (running && running->label) ? running->label : "");
    cJSON_AddStringToObject(root, "connection_type", get_connection_type_string());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_is_connected());
    cJSON_AddBoolToObject(root, "lan_connected", my_ethernet_is_connected());
    cJSON_AddStringToObject(root, "wifi_mode", "STA");
    cJSON_AddStringToObject(root, "wifi_ssid", (const char *)wifi_config.sta.ssid);
    cJSON_AddStringToObject(root, "ip_address", ip_str);
    cJSON_AddStringToObject(root, "mac_address", mac_str);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "dmx_channels", 512);

    char *json_str = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, (const char *)json_str, HTTPD_RESP_USE_STRLEN);

    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief POST /api/system/reboot - Reboot the device
 */
esp_err_t system_reboot_handler(httpd_req_t *req)
{
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "Restarting in 1 second...");
    
    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, (const char *)json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    cJSON_Delete(response);

    // Restart after delay
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// ==================== Public API ====================

/**
 * @brief Register WiFi REST API handlers
 */
void wifi_rest_register_handlers(httpd_handle_t server)
{
    // GET /api/wifi
    httpd_uri_t get_wifi_uri = {
        .uri = "/api/wifi",
        .method = HTTP_GET,
        .handler = wifi_get_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &get_wifi_uri);

    // POST /api/wifi
    httpd_uri_t post_wifi_uri = {
        .uri = "/api/wifi",
        .method = HTTP_POST,
        .handler = wifi_post_config_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &post_wifi_uri);

    // GET /api/system
    httpd_uri_t get_system_uri = {
        .uri = "/api/system",
        .method = HTTP_GET,
        .handler = system_info_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &get_system_uri);

    // POST /api/system/reboot
    httpd_uri_t reboot_uri = {
        .uri = "/api/system/reboot",
        .method = HTTP_POST,
        .handler = system_reboot_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &reboot_uri);

    ESP_LOGI(TAG, "WiFi REST API handlers registered");
}
