#include "my_ethernet.h"
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth_phy.h"
#include "esp_eth_mac.h"
#include "driver/gpio.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "my_ethernet";

// Event group for tracking Ethernet status
static EventGroupHandle_t eth_event_group = NULL;
#define ETH_CONNECTED_BIT BIT0
#define ETH_GOT_IP_BIT BIT1

static esp_eth_handle_t eth_handle = NULL;
static esp_netif_t *eth_netif = NULL;
static bool eth_initialized = false;
static bool eth_connected = false;
static char current_hostname[32] = "udp2dmx-eth";

// Event handler for Ethernet events
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id)
    {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        if (eth_event_group)
        {
            xEventGroupSetBits(eth_event_group, ETH_CONNECTED_BIT);
        }
        eth_connected = true;
        break;

    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        if (eth_event_group)
        {
            xEventGroupClearBits(eth_event_group, ETH_CONNECTED_BIT | ETH_GOT_IP_BIT);
        }
        eth_connected = false;
        break;

    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;

    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        if (eth_event_group)
        {
            xEventGroupClearBits(eth_event_group, ETH_CONNECTED_BIT | ETH_GOT_IP_BIT);
        }
        eth_connected = false;
        break;

    default:
        break;
    }
}

// Event handler for IP events
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_ETH_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        const esp_netif_ip_info_t *ip_info = &event->ip_info;

        ESP_LOGI(TAG, "Ethernet Got IP Address");
        ESP_LOGI(TAG, "~~~~~~~~~~~");
        ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
        ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
        ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
        ESP_LOGI(TAG, "~~~~~~~~~~~");

        if (eth_event_group)
        {
            xEventGroupSetBits(eth_event_group, ETH_GOT_IP_BIT);
        }

        // Initialize mDNS
        if (mdns_init() == ESP_OK)
        {
            mdns_hostname_set(current_hostname);
            mdns_instance_name_set(current_hostname);
            ESP_LOGI(TAG, "mDNS started with hostname: %s.local", current_hostname);
        }
        else
        {
            ESP_LOGW(TAG, "mDNS init failed");
        }
    }
}

esp_err_t my_ethernet_init(int mdc_gpio, int mdio_gpio, int phy_addr,
                          int phy_power_gpio, int phy_rst_gpio,
                          int clock_mode, const char *hostname)
{
    if (eth_initialized)
    {
        ESP_LOGW(TAG, "Ethernet already initialized");
        return ESP_OK;
    }

    // Save hostname
    if (hostname != NULL)
    {
        strncpy(current_hostname, hostname, sizeof(current_hostname) - 1);
        current_hostname[sizeof(current_hostname) - 1] = '\0';
    }

    // Create event group
    eth_event_group = xEventGroupCreate();
    if (eth_event_group == NULL)
    {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    // Configure PHY power pin if specified
    if (phy_power_gpio >= 0)
    {
        gpio_config_t pwr_cfg = {
            .pin_bit_mask = (1ULL << phy_power_gpio),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&pwr_cfg));
        gpio_set_level(phy_power_gpio, 1);
        ESP_LOGI(TAG, "PHY power enabled on GPIO%d", phy_power_gpio);
        vTaskDelay(pdMS_TO_TICKS(100)); // Wait for PHY to power up
    }

    // Configure PHY reset pin if specified
    if (phy_rst_gpio >= 0)
    {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = (1ULL << phy_rst_gpio),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&rst_cfg));
        
        // Reset sequence
        gpio_set_level(phy_rst_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(phy_rst_gpio, 1);
        ESP_LOGI(TAG, "PHY reset on GPIO%d", phy_rst_gpio);
        vTaskDelay(pdMS_TO_TICKS(100)); // Wait for PHY to come out of reset
    }

    // Create netif for Ethernet
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif = esp_netif_new(&netif_cfg);
    if (eth_netif == NULL)
    {
        ESP_LOGE(TAG, "Failed to create Ethernet netif");
        return ESP_FAIL;
    }

    // Set default handlers for Ethernet
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;

    // Configure MAC
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.smi_mdc_gpio_num = mdc_gpio;
    esp32_emac_config.smi_mdio_gpio_num = mdio_gpio;
    
    // Configure clock mode
    switch (clock_mode)
    {
    case 0: // GPIO0 Input
        esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
        esp32_emac_config.clock_config.rmii.clock_gpio = 0; // GPIO0 for input
        break;
    case 1: // GPIO0 Output
        esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
        esp32_emac_config.clock_config.rmii.clock_gpio = 0; // GPIO0 for output
        break;
    case 2: // GPIO16 Output
        esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
        esp32_emac_config.clock_config.rmii.clock_gpio = 16; // GPIO16
        break;
    case 3: // GPIO17 Output
        esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
        esp32_emac_config.clock_config.rmii.clock_gpio = 17; // GPIO17
        break;
    default:
        ESP_LOGW(TAG, "Invalid clock mode %d, using default (GPIO0 IN)", clock_mode);
        esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
        esp32_emac_config.clock_config.rmii.clock_gpio = 0;
        break;
    }

    mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    if (mac == NULL)
    {
        ESP_LOGE(TAG, "Failed to create MAC");
        return ESP_FAIL;
    }

    // Configure PHY (LAN8720A)
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = phy_addr;
    phy_config.reset_gpio_num = phy_rst_gpio;
    phy = esp_eth_phy_new_lan87xx(&phy_config);
    if (phy == NULL)
    {
        ESP_LOGE(TAG, "Failed to create PHY");
        return ESP_FAIL;
    }

    // Install Ethernet driver
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&eth_config, &eth_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    // Attach Ethernet driver to TCP/IP stack
    err = esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to attach netif: %s", esp_err_to_name(err));
        return err;
    }

    // Register event handlers
    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register ETH event handler: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(err));
        return err;
    }

    eth_initialized = true;
    ESP_LOGI(TAG, "Ethernet initialized (MDC=%d, MDIO=%d, PHY_ADDR=%d, CLK_MODE=%d)",
             mdc_gpio, mdio_gpio, phy_addr, clock_mode);

    return ESP_OK;
}

esp_err_t my_ethernet_start(void)
{
    if (!eth_initialized || eth_handle == NULL)
    {
        ESP_LOGE(TAG, "Ethernet not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_eth_start(eth_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start Ethernet: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Ethernet started");
    return ESP_OK;
}

esp_err_t my_ethernet_stop(void)
{
    if (!eth_initialized || eth_handle == NULL)
    {
        ESP_LOGE(TAG, "Ethernet not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_eth_stop(eth_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to stop Ethernet: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Ethernet stopped");
    return ESP_OK;
}

void my_ethernet_deinit(void)
{
    if (!eth_initialized)
    {
        return;
    }

    if (eth_handle != NULL)
    {
        esp_eth_stop(eth_handle);
        esp_eth_driver_uninstall(eth_handle);
        eth_handle = NULL;
    }

    if (eth_netif != NULL)
    {
        esp_netif_destroy(eth_netif);
        eth_netif = NULL;
    }

    esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler);

    if (eth_event_group != NULL)
    {
        vEventGroupDelete(eth_event_group);
        eth_event_group = NULL;
    }

    eth_initialized = false;
    eth_connected = false;
    ESP_LOGI(TAG, "Ethernet deinitialized");
}

bool my_ethernet_is_connected(void)
{
    if (!eth_initialized || eth_event_group == NULL)
    {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(eth_event_group);
    return (bits & ETH_GOT_IP_BIT) != 0;
}

bool my_ethernet_get_ip(char *ip_str)
{
    if (!eth_initialized || eth_netif == NULL || ip_str == NULL)
    {
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(eth_netif, &ip_info) == ESP_OK)
    {
        sprintf(ip_str, IPSTR, IP2STR(&ip_info.ip));
        return true;
    }

    return false;
}
