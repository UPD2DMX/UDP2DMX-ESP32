#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_eth.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Ethernet with LAN8720A PHY
 * 
 * @param mdc_gpio GPIO for MDC
 * @param mdio_gpio GPIO for MDIO
 * @param phy_addr PHY address (0-31)
 * @param phy_power_gpio GPIO for PHY power enable (-1 if not used)
 * @param phy_rst_gpio GPIO for PHY reset (-1 if not used)
 * @param clock_mode Clock mode: 0=GPIO0_IN, 1=GPIO0_OUT, 2=GPIO16_OUT, 3=GPIO17_OUT
 * @param hostname Hostname for mDNS
 * @return esp_err_t ESP_OK on success
 */
esp_err_t my_ethernet_init(int mdc_gpio, int mdio_gpio, int phy_addr,
                          int phy_power_gpio, int phy_rst_gpio,
                          int clock_mode, const char *hostname);

/**
 * @brief Deinitialize Ethernet
 */
void my_ethernet_deinit(void);

/**
 * @brief Check if Ethernet is connected
 * 
 * @return true if Ethernet link is up
 */
bool my_ethernet_is_connected(void);

/**
 * @brief Get Ethernet IP address
 * 
 * @param ip_str Buffer to store IP address string (minimum 16 bytes)
 * @return true if IP address is available
 */
bool my_ethernet_get_ip(char *ip_str);

/**
 * @brief Start Ethernet connection
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t my_ethernet_start(void);

/**
 * @brief Stop Ethernet connection
 * 
 * @return esp_err_t ESP_OK on success
 */
esp_err_t my_ethernet_stop(void);

#ifdef __cplusplus
}
#endif
