# ESP32-Digital-GW Board Configuration

## Übersicht

Die Pin-Konfiguration des UDP2DMX-ESP32-Projekts wird jetzt über die **Menuconfig (sdkconfig)** verwaltet und nicht über den Webserver. Dies ermöglicht die Erstellung von Board-spezifischen Konfigurationsdateien.

## Hardware-Pinzuordnung (ESP32-Digital-GW)

| Funktion | GPIO Pin | Beschreibung |
|----------|----------|--------------|
| DMX TX | GPIO 17 | RS485 DI (Data In) |
| DMX RX | GPIO 16 | RS485 RO (Receive Out) |
| DMX RTS | GPIO 18 | RS485 DE (Driver Enable) |
| Debug LED | GPIO 2 | Status-LED |

## Menuconfig Optionen

Die Hardware-Pins können über `idf.py menuconfig` unter dem Menü **Hardware Configuration** konfiguriert werden:

- **DMX TX GPIO Pin** (CONFIG_DMX_TX_GPIO) - Standard: 17
- **DMX RX GPIO Pin** (CONFIG_DMX_RX_GPIO) - Standard: 16
- **DMX RTS GPIO Pin** (CONFIG_DMX_RTS_GPIO) - Standard: 18
- **DMX UART Number** (CONFIG_DMX_UART_NUM) - Standard: 2
- **DMX Baudrate** (CONFIG_DMX_BAUDRATE) - Standard: 250000
- **WiFi Switch Button GPIO** (CONFIG_WIFI_SWITCH_BUTTON_GPIO) - Standard: 0 (deaktiviert)

## Board-spezifische sdkconfig Dateien

Für verschiedene Hardware-Boards sollten Board-spezifische sdkconfig-Dateien erstellt werden:

### Verwendung

```bash
# Standard-Build mit aktueller sdkconfig
idf.py build

# Build mit ESP32-Digital-GW Konfiguration
cp sdkconfig.esp32-digital-gw sdkconfig
idf.py build
```

### Verfügbare Board-Konfigurationen

- **sdkconfig.esp32-digital-gw** - ESP32-Digital-GW Hardware
  - DMX TX: GPIO 17
  - DMX RX: GPIO 16
  - DMX RTS: GPIO 18

## Neue Board-Konfiguration erstellen

Um eine neue Board-spezifische Konfiguration zu erstellen:

1. **Menuconfig öffnen:**
   ```bash
   idf.py menuconfig
   ```

2. **Hardware Configuration anpassen:**
   - Zu `Hardware Configuration` navigieren
   - Pins für das neue Board konfigurieren

3. **Konfiguration speichern:**
   - Speichern und Menuconfig beenden

4. **Board-Datei erstellen:**
   ```bash
   cp sdkconfig sdkconfig.<board-name>
   ```

5. **In Git einbinden:**
   ```bash
   git add sdkconfig.<board-name>
   git commit -m "Add sdkconfig for <board-name>"
   ```

## Integration in system_config.c

Die Hardware-Pin-Konfiguration wird beim Kompilieren automatisch aus der sdkconfig eingebunden:

```c
#include "sdkconfig.h"

static system_config_t default_config = {
    .hardware = {
        .dmx_tx_pin = CONFIG_DMX_TX_GPIO,      // Aus sdkconfig.h
        .dmx_rx_pin = CONFIG_DMX_RX_GPIO,      // Aus sdkconfig.h
        .dmx_en_pin = CONFIG_DMX_RTS_GPIO,     // Aus sdkconfig.h
        .debug_led_gpio = 2},
    // ... weitere Konfiguration ...
};
```

## Wichtige Hinweise

- ⚠️ **Keine Web-UI Konfiguration für Pins**: Die Pins werden nicht über den Webserver konfiguriert, sondern nur über Menuconfig
- ✅ **Zur Compile-Zeit festgelegt**: Die Pin-Konfiguration wird während des Compilierens festgelegt
- ✅ **Board-Portabilität**: Board-spezifische sdkconfig-Dateien ermöglichen einfaches Wechseln zwischen verschiedenen Hardware-Konfigurationen

## Webserver-Konfiguration

Der Webserver (`http://192.168.4.1` oder `http://udp2dmx.local` `http://[hostname].local`) verwaltet weiterhin:
- WiFi-Einstellungen (Netzwerkname und Passwort)
- Netzwerk-Konfiguration (IP, DHCP, DNS)
- DMX-Inhalte (via UDP-Protokoll)

Die Hardware-Pins werden **nicht** über den Webserver konfiguriert.

## Debugging

Die aktuelle Hardware-Konfiguration kann in den Logs überprüft werden:

```bash
idf.py monitor
```

Die Logs zeigen die konfigurierten Pins beim Start an.
