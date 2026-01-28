# 🌐 UDP2DMX Gateway (ESP32)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

A lightweight and flexible UDP-to-DMX gateway based on the ESP32 platform, designed to bridge the **Loxone Home Automation System** with **DMX512-based lighting systems** via UDP over Wi-Fi.

This project enables direct communication between Loxone and DMX devices using a well-defined, lightweight UDP protocol. It is ideal for smart home installations where wireless DMX control is required.

---

## ⚙️ Key Features

- ✅ Based on the open-source [ESP-DMX library](https://github.com/someweisguy/esp_dmx)  
- ✅ UDP-based protocol (compatible with Loxone UDP protocol)  
- ✅ Easily testable via Jupyter notebook  

---

## 🚀 Getting Started

### 1. Install ESP-IDF

Make sure ESP-IDF **v5.0** is installed and properly set up. For installation instructions, refer to the [official ESP-IDF documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html).

---

### 2. Clone the Repository

```bash
git clone "link to this Repo"
cd udp2dmx-esp32
```

---

### 3. Configure Project with `menuconfig`

Launch the ESP-IDF configuration menu:

```bash
idf.py menuconfig
```

Set the following options:

- 📶 **Component config → Wi-Fi Configuration**  
  - SSID and password for up to 3 different Wi-Fi networks
  - GPIO pin for network selection button (default: GPIO 0)

---

### 4. Advanced Configuration via REST API

After the device is running, you can configure advanced settings via the built-in REST API:

#### 📋 Configuration File Structure

The device uses a `config.json` file stored in SPIFFS with the following structure:

```json
{
    "hostname": "udp2dmx2",
    "ct_config": {
        "1": 2700,
        "2": 6500,
        "5": 3000
    },
    "default_ct": {
        "min": 3400,
        "max": 6600
    }
}
```

#### 🌐 REST API Endpoints

| Method  | Endpoint        | Description                          |
| ------- | --------------- | ------------------------------------ |
| `GET`   | `/config`       | Retrieve current configuration       |
| `POST`  | `/config`       | Replace entire configuration         |
| `PATCH` | `/config/patch` | Update specific configuration values |

#### 📝 Configuration Options

- **`hostname`**: Device hostname for network identification
- **`ct_config`**: Color temperature mapping for specific channels (channel → Kelvin)
- **`default_ct`**: Default color temperature range for DMXL commands
  - `min`: Minimum color temperature in Kelvin
  - `max`: Maximum color temperature in Kelvin

#### 💡 Example Usage

```bash
# Get current configuration
curl http://192.168.1.100/config

# Update hostname
curl -X PATCH http://192.168.1.100/config/patch \
  -H "Content-Type: application/json" \
  -d '{"hostname": "my-dmx-gateway"}'

# Set color temperature for channel 3
curl -X PATCH http://192.168.1.100/config/patch \
  -H "Content-Type: application/json" \
  -d '{"ct_config": {"3": 4000}}'
```

---

### 5. Build and Flash Firmware

```bash
idf.py build
idf.py flash
idf.py spiffs-flash
```

---

### 6. Monitor Output
*Optional*

```bash
idf.py monitor
```

After flashing, the device will connect to Wi-Fi and output its IP address via serial console. Note this IP for testing.

---

## 🧪 DMX Communication Test
*Optional*

You can use the included **`testudp.ipynb`** Jupyter Notebook to send test UDP commands:

1. Open the notebook in Jupyter
2. Set the IP address of your ESP32 device
3. Send UDP packets to test DMX channels
4. Watch serial monitor logs or connected DMX lights to verify output

💡 **Tip**: The status LED (if configured) will blink to confirm incoming UDP traffic.

---

## 🏠 Loxone Integration

To use the gateway in your **Loxone Config**:

1. Use **Virtual Outputs** to send UDP commands to the ESP32's IP.
2. Format the command according to the protocol (see below).
3. Configure each DMX channel or RGB output as needed.

---

## 📡 UDP Protocol Overview

The device expects UDP packets in the following formats:

| Type  | Format                               | Description                                                                                                                                   |
| ----- | ------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------- |
| **P** | `DMXP<ch>#<percent>#<fade>`          | Set a single channel to a value from 0–100% with optional fade (converted to 0–255 internally).                                               |
| **C** | `DMXC<ch>#<value>#<fade>`            | Set a single channel (0–255) with optional fade time.                                                                                         |
| **R** | `DMXR<ch>#<rgb>#<fade>`              | Set 3 consecutive channels for RGB values. Format: `RRRGGGBBB` (e.g., `128128128`).                                                           |
| **W** | `DMXW<ch>#<wwcw>#<fade>`             | Set 2 consecutive channels for Tunable White. Format: `WWWCCC` (e.g., `200050` = WW:200, CW:50).                                              |
| **L** | `DMXL<ch>#20<brightness><CT>#<fade>` | Set brightness and color temperature. Can be used with the Lumitech type from Loxone Format: `20BBBTTTT` (e.g., `200507000` = 50% at 7000 K). |

---

## 🔆 LED Behavior – Summary

| Condition                               | LED Behavior                | Description                                   |
| --------------------------------------- | --------------------------- | --------------------------------------------- |
| **Connected (LAN or WiFi)**<br>**No DMX error** | **Off (constantly)**        | Normal operation                              |
| **Not connected** (LAN & WiFi)          | **Slow blinking**           | 500 ms on / 500 ms off                        |
| **DMX error active**                    | **Fast blinking**           | 100 ms on / 100 ms off                        |
| **User action**<br>(e.g. Wi-Fi switch)  | **Short blinking sequence** | `n` blinks with `delay_ms` (e.g. 2× 50 ms)    |
| **Connecting to LAN**                   | **3 short blinks**          | Signals successful LAN connection             |
| **Connecting to WiFi**                  | **2 short blinks**          | Signals successful WiFi connection            |
| **Receiving DMX data**                  | **1 short blink**           | Brief signal when DMX packet is received      |

---

## 📁 Project Structure

```
main/
├── include/                     # Public header files
│   ├── dmx_manager.h           # DMX hardware abstraction
│   ├── udp_protocol.h          # UDP protocol handling
│   ├── udp_server.h            # UDP server implementation
│   └── system_config.h         # System configuration
├── src/                        # Source files
│   ├── main.c                  # Application entry point
│   ├── dmx_manager.c           # DMX management & fade engine
│   ├── udp_protocol.c          # Protocol parsing & execution
│   ├── udp_server.c            # UDP server & packet handling
│   └── system_config.c         # Configuration management
└── CMakeLists.txt              # Build configuration

components/
├── my_wifi/                    # WiFi management
├── my_led/                     # LED status indication
├── my_config/                  # Configuration (CT values, hostname)
└── config_handler/             # REST API for configuration
```

---

## 📋 Requirements

- ESP32 board (e.g., ESP32 DevKitC)
- RS485 transceiver (e.g., MAX485) I am using: MAX 13487
- DMX-compatible light or fixture
- Python + Jupyter (optional, for testing)

---

## 🙏 Credits

- [ESP-DMX Library](https://github.com/someweisguy/esp_dmx) by [@someweisguy](https://github.com/someweisguy)
- **Robert Lechner** for documenting the UDP DMX protocol

---

## 💖 Support

If this project helps you, consider buying me a coffee! ☕

[![PayPal](https://img.shields.io/badge/PayPal-00457C?style=for-the-badge&logo=paypal&logoColor=white)](https://paypal.me/kai009)

---

## 📃 License

MIT License — see `LICENSE` file for details.
