# Project Voyager 🚀

<div align="center">

**Long-Range Wireless File Transfer System for ESP32**

[![Platform](https://img.shields.io/badge/platform-ESP32-blue.svg)](https://www.espressif.com/en/products/socs/esp32)
[![Framework](https://img.shields.io/badge/framework-Arduino-00979D.svg)](https://www.arduino.cc/)
[![LoRa](https://img.shields.io/badge/LoRa-433MHz-green.svg)](https://lora-alliance.org/)
[![License](https://img.shields.io/badge/license-MIT-brightgreen.svg)](LICENSE)

*Enabling offline, internet-free file sharing across kilometers using LoRa technology*

</div>

---

## 📖 Overview

Project Voyager is an ESP32-based wireless communication platform that enables file and data transfer over long distances using LoRa (Long Range) radio technology. Unlike traditional file sharing methods that rely on WiFi or cellular networks, Voyager operates completely offline, making it ideal for:

- 🏔️ Remote locations without internet connectivity
- 🔒 Privacy-focused scenarios requiring no third-party networks
- 🏭 Industrial environments with network restrictions
- 🚁 IoT deployments across large geographical areas
- 📡 Emergency communications and disaster recovery

### Key Features

#### 🎯 Core Capabilities
- **Long-Range Communication**: Transfer files up to several kilometers using LoRa (433 MHz)
- **Dual Operating Modes**: Access Point (AP) or Station (STA) WiFi connectivity
- **Web-Based Interface**: Intuitive dashboard for device control and monitoring
- **Real-Time Updates**: WebSocket-based live status notifications
- **MQTT Integration**: Bridge LoRa messages to MQTT brokers for IoT ecosystems
- **Reliable Transmission**: CRC8 checksum validation with automatic retry mechanism
- **OTA Updates**: Over-the-air firmware and filesystem updates

#### 💾 File System & Storage
- **LittleFS**: Modern flash filesystem with superior reliability
- **Persistent Configuration**: JSON-based settings stored in flash memory
- **Flexible Structure**: Organized config system (WiFi, LoRa, MQTT, user data)

#### 🔐 Communication Protocols
- **HTTP REST API**: Programmatic control and file submission
- **WebSocket**: Real-time bidirectional communication
- **Server-Sent Events (SSE)**: Push notifications to web clients
- **MQTT**: Optional message broker integration
- **LoRa Protocol**: Custom packet structure with ACK/NACK handling

---

## 🏗️ Architecture

### Hardware Components

```
┌─────────────────────────────────────────┐
│              ESP32 MCU                  │
│  ┌──────────────────────────────────┐  │
│  │  WiFi/BLE  │  240MHz Dual-Core   │  │
│  └──────────────────────────────────┘  │
└───────────┬─────────────────────────────┘
            │
      ┌─────┴──────┬──────────┬──────────┐
      │            │          │          │
┌─────▼─────┐ ┌───▼────┐ ┌──▼─────┐ ┌──▼─────┐
│ LoRa SX127│ │ OLED   │ │ Buttons│ │  LED   │
│ (433 MHz) │ │SSD1306 │ │  x2    │ │        │
└───────────┘ └────────┘ └────────┘ └────────┘
```

**Required Hardware:**
- ESP32 Development Board (ESP32-WROOM-32)
- LoRa Transceiver Module (SX1276/77/78/79 based)
- 0.96" OLED Display (SSD1306, 128x64, I2C)
- 2x Push Buttons
- 1x LED
- Antenna (433 MHz)

### Pin Configuration

#### Version 1 (esp_lora_v1)
| Component | GPIO Pin |
|-----------|----------|
| LoRa SCK  | 18       |
| LoRa MISO | 19       |
| LoRa MOSI | 23       |
| LoRa NSS  | 5        |
| LoRa RST  | 14       |
| LoRa DIO0 | 2        |
| OLED SCL  | 22       |
| OLED SDA  | 21       |
| Button 1  | 32       |
| Button 2  | 36       |
| LED       | 4        |

#### Version 2 (esp_lora_v2)
| Component | GPIO Pin |
|-----------|----------|
| LoRa SCK  | 5        |
| LoRa MISO | 19       |
| LoRa MOSI | 27       |
| LoRa NSS  | 18       |
| LoRa RST  | 23       |
| LoRa DIO0 | 26       |
| OLED SCL  | 22       |
| OLED SDA  | 21       |
| Button 1  | 39       |
| Button 2  | 36       |
| LED       | 2        |

### Software Architecture

```
┌───────────────────────────────────────────────────────┐
│                   Web Interface                        │
│  (HTML/CSS/JavaScript + WebSocket Client)             │
└─────────────────────┬─────────────────────────────────┘
                      │ HTTP/WebSocket
┌─────────────────────▼─────────────────────────────────┐
│              ESP32 Application Layer                   │
│  ┌──────────────┐  ┌──────────────┐  ┌─────────────┐ │
│  │ Web Server   │  │  WebSocket   │  │  MQTT Client││ │
│  │ (AsyncTCP)   │  │  Handler     │  │  (Optional) ││ │
│  └──────┬───────┘  └──────┬───────┘  └──────┬──────┘ │
│         └──────────────────┴──────────────────┘        │
└───────────────────────┬───────────────────────────────┘
                        │ FreeRTOS Queues
┌───────────────────────▼───────────────────────────────┐
│            Communication Task Layer                    │
│  ┌──────────────┐  ┌──────────────┐  ┌─────────────┐ │
│  │ LoRa TX Task │  │ LoRa RX Task │  │  WiFi Task  │ │
│  │ (Send Queue) │  │ (Recv Queue) │  │  (Monitor)  │ │
│  └──────┬───────┘  └──────┬───────┘  └─────────────┘ │
│         │                  │                           │
└─────────┼──────────────────┼───────────────────────────┘
          │                  │
┌─────────▼──────────────────▼───────────────────────────┐
│               Hardware Abstraction Layer               │
│  ┌──────────────┐  ┌──────────────┐  ┌─────────────┐ │
│  │  LoRa Radio  │  │   LittleFS   │  │  WiFi Stack │ │
│  │   (SPI)      │  │  Filesystem  │  │             │ │
│  └──────────────┘  └──────────────┘  └─────────────┘ │
└───────────────────────────────────────────────────────┘
```

### LoRa Protocol

**Packet Structure:**
```
┌────────┬──────┬──────────┬─────────────────┐
│ Length │ Type │ Checksum │   Data Payload  │
│ (1B)   │ (1B) │ (CRC8)   │   (Variable)    │
└────────┴──────┴──────────┴─────────────────┘
```

**Message Types:**
- `0x00` - LORA_MSG: Text messages
- `0x01` - RAW_DATA: File chunks / binary data
- `0x02` - REC_AKNG: Acknowledgment packets
- `0x03` - LORA_SERIAL: Serial communication

**Transmission Flow:**
```
Sender                          Receiver
  │                                 │
  ├──────────── PACKET ────────────>│
  │                                 │
  │                       ┌─────────▼────────┐
  │                       │ Validate Checksum│
  │                       └─────────┬────────┘
  │<───────────── ACK ───────────────┤
  │              (1=OK, 0=Retry)    │
  │                                 │
  └─ [Retry up to 3x on timeout]   │
```

---

## 🚀 Getting Started

### Prerequisites

- **PlatformIO IDE** (VSCode extension recommended) or **Arduino IDE**
- **USB Cable** for ESP32 programming
- **Python 3.x** (for esptool.py, if manual flashing)
- Basic understanding of embedded systems

### Installation

#### Using PlatformIO (Recommended)

1. **Clone the repository:**
   ```bash
   git clone https://github.com/aviralverma-8877/Project-Voyager.git
   cd Project-Voyager
   ```

2. **Open in PlatformIO:**
   - Open VSCode
   - Install PlatformIO IDE extension
   - File → Open Folder → Select `Project-Voyager`

3. **Select your hardware version:**
   - Edit `platformio.ini` if needed
   - Default environments: `esp_lora_v1` or `esp_lora_v2`

4. **Build and upload:**
   ```bash
   # Build firmware
   pio run -e esp_lora_v1

   # Upload to ESP32
   pio run -e esp_lora_v1 --target upload

   # Upload filesystem (LittleFS)
   pio run -e esp_lora_v1 --target uploadfs
   ```

5. **Monitor serial output:**
   ```bash
   pio device monitor -b 115200
   ```

#### Automated Build Scripts (Web Flasher)

For building all firmware versions and preparing web flasher files:

**Windows:**
```bash
build_webflash.bat
```

**Linux/Mac/Windows (Python):**
```bash
python build_webflash.py
```

This script will:
- Build firmware for both hardware versions (v1 and v2)
- Build LittleFS filesystem
- Copy all files to `webflash/` directory
- Prepare files for ESP Web Tools flasher

**Using Makefile (Linux/Mac):**
```bash
make webflash      # Build and prepare webflash binaries
make build         # Build all versions
make upload-v1     # Upload version 1
make monitor       # Serial monitor
```

See [webflash/README.md](webflash/README.md) for web flasher setup.

#### Using esptool.py (Manual Flashing)

```bash
# Flash firmware and filesystem
esptool.py --after hard_reset write_flash \
  0x1000 bootloader.bin \
  0x8000 partitions.bin \
  0xE000 boot_app0.bin \
  0x10000 firmware.bin \
  0x310000 littlefs.bin
```

**Note:** Replace `0x310000` with your partition address (check `partitions.csv`).

---

## ⚙️ Configuration

### First-Time Setup

1. **Power on the device** - It will create an Access Point (AP)
2. **Connect to WiFi AP:**
   - SSID: `Voyager` (or device MAC address)
   - Password: (none)
3. **Open web browser:**
   - Navigate to `http://192.168.4.1`
   - You'll see the Voyager dashboard

### Configuration Files

All settings are stored in `/config/` directory on LittleFS:

#### WiFi Configuration (`wifi_config.json`)
```json
{
  "wifi_function": "STA",
  "wifi_ssid": "YourWiFiNetwork",
  "wifi_pass": "YourPassword"
}
```
- `wifi_function`: `"AP"` (Access Point) or `"STA"` (Station mode)

#### LoRa Configuration (`lora_config.json`)
```json
{
  "freq": 433000000,
  "TxPower": 17,
  "SpreadingFactor": 7,
  "SignalBandwidth": 125000,
  "CodingRate4": 5,
  "SyncWord": 0x12
}
```
- `freq`: Frequency in Hz (e.g., 433 MHz = 433000000)
- `TxPower`: Transmission power (2-20 dBm)
- `SpreadingFactor`: 6-12 (higher = longer range, slower)
- `SignalBandwidth`: 7800-500000 Hz
- `CodingRate4`: 5-8 (error correction strength)

#### MQTT Configuration (`mqtt_config.json`)
```json
{
  "mqtt_enabled": true,
  "host": "192.168.1.100",
  "port": 1883,
  "auth": false,
  "username": "",
  "password": "",
  "pub_topic": "tx",
  "sub_topic": "rx",
  "raw_data": "send_raw",
  "ping_topic": "ping"
}
```

#### User Data (`user_data.json`)
```json
{
  "username": "Voyager_Device_01"
}
```

---

## 📡 Usage

### Web Interface

Access the web dashboard at:
- **AP Mode**: `http://192.168.4.1`
- **STA Mode**: `http://project-voyager-<MAC>.local` (mDNS) or device IP

**Dashboard Pages:**
- **Home** (`/`) - Main dashboard with device status
- **File Transfer** (`/file_transfer.html`) - Upload/send files via LoRa
- **LoRa Config** (`/lora.html`) - Configure LoRa parameters
- **WiFi Config** (`/wifi.html`) - Connect to WiFi networks
- **MQTT Config** (`/mqtt.html`) - MQTT broker settings
- **Firmware Update** (`/update`) - OTA firmware updates

### API Endpoints

#### Send LoRa Message
```bash
curl -X POST http://192.168.4.1/lora_transmit \
  -d "message=Hello Voyager"
```

#### Send Raw Data (File Chunk)
```bash
curl -X POST http://192.168.4.1/send_raw \
  -d "base64EncodedData"
```

#### Get Device Info
```bash
# Hostname
curl http://192.168.4.1/hostname

# Username
curl http://192.168.4.1/username
```

#### Device Control
```bash
# Restart device
curl http://192.168.4.1/restart

# Factory reset
curl http://192.168.4.1/reset
```

### WebSocket Communication

Connect to `ws://[device-ip]/ws` for real-time updates:

**Send command:**
```json
{
  "request-type": "wifi_ssid_scan"
}
```

**Receive updates:**
```json
{
  "response_type": "lora_rx",
  "lora_msg": "Received message content"
}
```

---

## 🔧 Advanced Configuration

### LoRa Parameters Tuning

For **maximum range** (slower speed):
```json
{
  "SpreadingFactor": 12,
  "SignalBandwidth": 125000,
  "CodingRate4": 8,
  "TxPower": 20
}
```

For **faster transmission** (shorter range):
```json
{
  "SpreadingFactor": 6,
  "SignalBandwidth": 500000,
  "CodingRate4": 5,
  "TxPower": 17
}
```

### Performance Optimization

The codebase includes several recent optimizations:

1. **Non-blocking ACK system** - Semaphore-based synchronization prevents task starvation
2. **Memory-efficient string handling** - Pre-allocated buffers reduce fragmentation
3. **Queue backpressure** - Gracefully handles overload without rebooting
4. **Mutex-protected LoRa access** - Prevents race conditions
5. **ISR-safe queue operations** - Proper FreeRTOS primitives

### MQTT Integration

When MQTT is enabled, Voyager bridges LoRa messages to MQTT topics:

**Topic Structure:**
```
voyager/<MAC_ADDRESS>/<pub_topic>/LORA_MSG
voyager/<MAC_ADDRESS>/<pub_topic>/RAW_DATA
voyager/<MAC_ADDRESS>/<sub_topic>
voyager/<MAC_ADDRESS>/send_raw
voyager/<MAC_ADDRESS>/ping
```

**Example Flow:**
```
LoRa RX → Voyager → MQTT Publish → Remote System
Remote System → MQTT Subscribe → Voyager → LoRa TX
```

---

## 🛠️ Development

### Project Structure

```
Project-Voyager/
├── src/
│   ├── main.cpp              # Application entry point
│   ├── lora_support.cpp/h    # LoRa transmission/reception
│   ├── web_server.cpp/h      # HTTP API endpoints
│   ├── web_sockets.cpp/h     # WebSocket handlers
│   ├── wifi_support.cpp/h    # WiFi management
│   ├── mqtt_support.cpp/h    # MQTT client
│   ├── support_method.cpp/h  # Utility functions
│   ├── oled_display.cpp/h    # OLED UI
│   └── tasks.cpp/h           # FreeRTOS task definitions
├── data_main/                # Web interface files
│   ├── index.html
│   ├── dashboard.html
│   ├── file_transfer.html
│   ├── script.js
│   └── config/*.json         # Configuration templates
├── platformio.ini            # Build configuration
└── README.md                 # This file
```

### Building from Source

1. **Install dependencies:**
   ```bash
   # Libraries are auto-downloaded by PlatformIO
   # See platformio.ini [lib_deps] section
   ```

2. **Customize build flags:**
   Edit `platformio.ini` to change GPIO pins, baud rate, etc.

3. **Build:**
   ```bash
   pio run -e esp_lora_v1
   ```

### Adding Custom Features

#### Example: Add new API endpoint

**In `web_server.cpp`:**
```cpp
server.on("/custom_endpoint", HTTP_GET, [](AsyncWebServerRequest *request) {
  request->send(200, "text/plain", "Custom response");
});
```

#### Example: Add new WebSocket command

**In `support_method.cpp` → `handle_operations()`:**
```cpp
if(strcmp(request_type, "custom_command") == 0) {
  // Your logic here
  send_to_ws("{\"response_type\":\"custom_response\"}");
}
```

---

## 📊 Performance Metrics

| Metric | Typical Value |
|--------|---------------|
| **LoRa Range** | 2-10 km (line of sight) |
| **Data Rate** | 0.3 - 5.5 kbps (depends on SF) |
| **ACK Timeout** | 5 seconds (configurable) |
| **Max Retries** | 3 attempts |
| **Queue Capacity** | 20 packets (send/receive) |
| **Task Stack Size** | 6000 bytes per task |
| **WiFi Modes** | AP + STA |
| **Max WebSocket Clients** | Limited by ESP32 memory (~10) |

---

## 🐛 Troubleshooting

### Common Issues

#### 1. **"LittleFS Mount Failed"**
- **Cause**: Filesystem not formatted or corrupted
- **Solution**:
  ```bash
  pio run -e esp_lora_v1 --target erase
  pio run -e esp_lora_v1 --target uploadfs
  ```

#### 2. **LoRa Transmission Fails**
- **Cause**: Incorrect frequency or poor antenna connection
- **Solution**:
  - Check antenna is properly connected
  - Verify frequency matches regulations (433/868/915 MHz)
  - Test with shorter distance first

#### 3. **Cannot Connect to AP**
- **Cause**: Device in STA mode or IP conflict
- **Solution**:
  - Factory reset (hold button or `/reset` endpoint)
  - Check OLED display for current mode

#### 4. **WebSocket Disconnects**
- **Cause**: Too many clients or memory exhaustion
- **Solution**:
  - Limit concurrent connections
  - Check serial monitor for heap info

### Debug Mode

Enable verbose logging:
```cpp
// In platformio.ini
-D DEBUG=true
-D DEBUGGING=true
```

Monitor serial output:
```bash
pio device monitor -b 115200 --filter=esp32_exception_decoder
```

---

## 🗺️ Roadmap

### Current Status: v1.0 (Stable)
- ✅ Basic file transfer over LoRa
- ✅ Web-based configuration interface
- ✅ MQTT integration
- ✅ OTA updates
- ✅ Performance optimizations (Dec 2024)

### Planned Features: v2.0
- 🔲 File encryption (AES-256)
- 🔲 Multi-device broadcasting
- 🔲 Directory synchronization
- 🔲 Progressive file transfer (resume support)
- 🔲 BLE configuration interface
- 🔲 Mobile app (Android/iOS)
- 🔲 Mesh networking support

---

## 🤝 Contributing

Contributions are welcome! Please follow these guidelines:

1. **Fork the repository**
2. **Create a feature branch:**
   ```bash
   git checkout -b feature/amazing-feature
   ```
3. **Commit your changes:**
   ```bash
   git commit -m "Add amazing feature"
   ```
4. **Push to the branch:**
   ```bash
   git push origin feature/amazing-feature
   ```
5. **Open a Pull Request**

### Code Style
- Follow existing naming conventions
- Comment complex logic
- Test on actual hardware before submitting

---

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---

## 🙏 Acknowledgments

### Libraries Used
- [sandeepmistry/LoRa](https://github.com/sandeepmistry/arduino-LoRa) - LoRa radio library
- [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer) - Async HTTP server
- [AsyncMqttClient](https://github.com/marvinroger/async-mqtt-client) - MQTT client
- [ArduinoJson](https://arduinojson.org/) - JSON parsing
- [Adafruit SSD1306](https://github.com/adafruit/Adafruit_SSD1306) - OLED display

### Inspiration
- LoRa community projects
- ESP32 open-source ecosystem
- Off-grid communication systems

---

## 📧 Contact & Support

- **Issues**: [GitHub Issues](https://github.com/aviralverma-8877/Project-Voyager/issues)
- **Discussions**: [GitHub Discussions](https://github.com/aviralverma-8877/Project-Voyager/discussions)
- **Email**: aviral.verma.8877@gmail.com

---

<div align="center">

**⭐ Star this repository if you find it useful!**

Made with ❤️ for the IoT and maker community

</div>
