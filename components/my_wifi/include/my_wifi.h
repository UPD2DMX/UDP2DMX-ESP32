#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief WiFi Konfiguration für NVS
     */
    typedef struct {
        char ssid[32];              // Primary WiFi SSID
        char password[64];          // Primary WiFi Password
        uint8_t auth_mode;          // WiFi Auth Mode
        bool use_dhcp;              // true = DHCP, false = static IP
        uint32_t static_ip;         // Static IP (host byte order)
        uint32_t subnet_mask;       // Subnet Mask
        uint32_t gateway;           // Gateway IP
        uint32_t dns;               // DNS Server
    } wifi_config_nvs_t;

    // Core WiFi Functions
    void my_wifi_init(void);
    bool my_wifi_is_connected(void);
    void my_wifi_set_hostname(const char *new_hostname);

    // NVS WiFi Configuration Functions
    /**
     * @brief Speichert primäre WiFi-Konfiguration in NVS
     * @param config Pointer zur WiFi-Konfiguration
     * @return true wenn erfolgreich, false bei Fehler
     */
    bool my_wifi_nvs_save_config(const wifi_config_nvs_t *config);

    /**
     * @brief Lädt primäre WiFi-Konfiguration aus NVS
     * @param config Pointer zu WiFi-Konfiguration (wird gefüllt)
     * @return true wenn Config existiert, false wenn nicht vorhanden
     */
    bool my_wifi_nvs_load_config(wifi_config_nvs_t *config);

    /**
     * @brief Löscht primäre WiFi-Konfiguration aus NVS
     * @return true wenn erfolgreich
     */
    bool my_wifi_nvs_delete_config(void);

    /**
     * @brief Startet Config-WiFi (AP-Mode)
     * @param ssid SSID des Config-WiFi
     * @param password Passwort (NULL für offenes Netzwerk)
     */
    void my_wifi_start_config_mode(const char *ssid, const char *password);

    /**
     * @brief Versucht sich mit primärem WiFi zu verbinden
     * @param config WiFi-Konfiguration
     * @param timeout_ms Timeout in Millisekunden
     * @return true wenn erfolgreich verbunden
     */
    bool my_wifi_connect_to_primary(const wifi_config_nvs_t *config, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif