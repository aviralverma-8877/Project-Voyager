# Project Voyager - Quick Start Guide

Get your ESP32-LoRa device up and running in minutes!

## 🚀 Choose Your Method

### Method 1: Pre-Built Binaries (Easiest)

**Best for:** Users who want to flash immediately without building

1. **Download pre-built binaries** from [Releases](https://github.com/aviralverma-8877/Project-Voyager/releases)

2. **Flash using ESP Web Tools** (Chrome/Edge browser)
   - Go to the web flasher page
   - Connect ESP32 via USB
   - Click "Flash" button
   - Select your hardware version (v1 or v2)

3. **Done!** Device will reboot and create WiFi AP "Voyager"

### Method 2: Build from Source (Recommended)

**Best for:** Developers who want to customize or contribute

#### Prerequisites
- [PlatformIO](https://platformio.org/install) installed
- USB cable for ESP32
- Python 3.x (optional, for build scripts)

#### Steps

1. **Clone the repository**
   ```bash
   git clone https://github.com/aviralverma-8877/Project-Voyager.git
   cd Project-Voyager
   ```

2. **Build everything** (automated)

   **Windows:**
   ```bash
   build_webflash.bat
   ```

   **Linux/Mac:**
   ```bash
   python build_webflash.py
   # or
   make webflash
   ```

3. **Flash to ESP32**
   ```bash
   # For hardware version 1
   pio run -e esp_lora_v1 --target upload
   pio run -e esp_lora_v1 --target uploadfs

   # For hardware version 2
   pio run -e esp_lora_v2 --target upload
   pio run -e esp_lora_v2 --target uploadfs
   ```

4. **Monitor serial output**
   ```bash
   pio device monitor -b 115200
   ```

### Method 3: Manual Build (Advanced)

For step-by-step control:

```bash
# 1. Build firmware for version 1
pio run -e esp_lora_v1

# 2. Build filesystem
pio run -e esp_lora_v1 --target buildfs

# 3. Flash firmware
pio run -e esp_lora_v1 --target upload

# 4. Flash filesystem
pio run -e esp_lora_v1 --target uploadfs
```

---

## 🔧 First-Time Setup

### 1. Power On

After flashing, the device will:
- Boot with splash screen animation
- Create WiFi Access Point named **"Voyager"**
- Display status on OLED screen

### 2. Connect to WiFi

**On your phone/computer:**
- Connect to WiFi network: **Voyager**
- No password required
- Wait for connection

### 3. Open Web Interface

- Open browser
- Go to: **http://192.168.4.1**
- You'll see the Project Voyager dashboard

### 4. Configure WiFi (Optional)

To connect device to your WiFi network:

1. Click **"WiFi Config"** in web interface
2. Click **"Scan Networks"**
3. Select your network
4. Enter password
5. Click **"Connect"**
6. Device will reboot and connect

**After connecting to STA mode:**
- Access via mDNS: `http://project-voyager-<MAC>.local`
- Or use the IP address shown on OLED display

### 5. Configure LoRa

1. Click **"LoRa Config"**
2. Set frequency (433/868/915 MHz based on your region)
3. Adjust settings for range vs. speed:
   - **Long Range:** SF=12, BW=125kHz
   - **Fast:** SF=6, BW=500kHz
4. Click **"Save"**

### 6. Test Communication

**On first device:**
1. Go to **"File Transfer"** page
2. Type a message
3. Click **"Send"**

**On second device:**
- Message should appear in the received messages area
- OLED will show "Receiving" status

---

## 📡 Hardware Versions

Make sure you flash the correct version for your hardware!

### How to Identify Your Hardware Version

Check your LoRa module connections:

**Version 1:**
- LoRa connected to: GPIO 18 (SCK), 19 (MISO), 23 (MOSI), 5 (NSS)
- LED on GPIO 4
- Use `esp_lora_v1` environment

**Version 2:**
- LoRa connected to: GPIO 5 (SCK), 19 (MISO), 27 (MOSI), 18 (NSS)
- LED on GPIO 2
- Use `esp_lora_v2` environment

**Both versions:**
- OLED: GPIO 22 (SCL), 21 (SDA)
- Buttons: GPIO 32/39 (BTN1), 36 (BTN2)

---

## 🎯 Quick Commands Reference

### Build Commands
```bash
# Build all versions
make build
# or
pio run -e esp_lora_v1 && pio run -e esp_lora_v2

# Build specific version
make build-v1      # Version 1
make build-v2      # Version 2

# Build filesystem
make buildfs
# or
pio run -e esp_lora_v1 --target buildfs
```

### Upload Commands
```bash
# Upload firmware
make upload-v1     # Version 1
make upload-v2     # Version 2

# Upload filesystem
make uploadfs
# or
pio run -e esp_lora_v1 --target uploadfs

# Upload both (firmware + filesystem)
make full-v1       # Version 1
make full-v2       # Version 2
```

### Monitor Commands
```bash
# Serial monitor
make monitor
# or
pio device monitor -b 115200

# Monitor with ESP32 exception decoder
pio device monitor -b 115200 --filter=esp32_exception_decoder
```

### Clean Commands
```bash
# Clean build artifacts
make clean
# or
pio run --target clean

# Full clean (including .pio directory)
rm -rf .pio
```

---

## 🐛 Troubleshooting

### Device Not Detected

**Windows:**
- Install [CP210x drivers](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
- Check Device Manager for COM port

**Linux:**
- Add user to dialout group: `sudo usermod -a -G dialout $USER`
- Log out and back in

**Mac:**
- Usually works out of the box
- If not, install CP210x drivers

### Upload Failed

1. **Hold BOOT button** during upload
2. Try different USB cable (data cable, not charge-only)
3. Check ESP32 is powered properly
4. Verify correct COM port: `pio device list`

### Build Failed

1. **Update PlatformIO:**
   ```bash
   pio upgrade
   pio pkg update
   ```

2. **Clean and rebuild:**
   ```bash
   pio run --target clean
   pio run -e esp_lora_v1
   ```

3. **Check dependencies:**
   ```bash
   pio pkg install
   ```

### LoRa Not Working

1. **Check antenna** is connected
2. **Verify frequency** matches your region:
   - Europe: 868 MHz
   - US: 915 MHz
   - Asia: 433 MHz
3. **Check LoRa module** connections (SPI pins)
4. **Try different spreading factor** (start with SF=7)

### WiFi Connection Issues

1. **Factory reset** device (hold button or use `/reset` endpoint)
2. **Check SSID/password** are correct
3. **Signal strength** - move closer to router
4. **Try AP mode** if STA fails

### OLED Display Blank

1. **Check I2C connections** (SDA=21, SCL=22)
2. **Verify I2C address** (0x3C or 0x3D)
3. **Check serial monitor** for error messages
4. **Try different OLED module**

---

## 📱 Mobile App (Optional)

For advanced users, you can create a mobile app to control the device:

1. Use the REST API endpoints (documented in README)
2. Connect via WebSocket for real-time updates
3. MQTT integration for remote control

---

## 🔗 Useful Links

- **Documentation:** [README.md](README.md)
- **API Reference:** See README.md → Usage section
- **Web Flasher Guide:** [webflash/README.md](webflash/README.md)
- **OLED Display Guide:** [OLED_DISPLAY_USAGE.md](OLED_DISPLAY_USAGE.md)
- **Changelog:** [CHANGELOG.md](CHANGELOG.md)

---

## 💬 Get Help

- **Issues:** https://github.com/aviralverma-8877/Project-Voyager/issues
- **Discussions:** https://github.com/aviralverma-8877/Project-Voyager/discussions
- **Email:** aviral.verma.8877@gmail.com

---

## ✨ Next Steps

After getting your device running:

1. **Configure MQTT** for IoT integration
2. **Test file transfer** between two devices
3. **Customize OLED display** (see OLED_DISPLAY_USAGE.md)
4. **Explore REST API** endpoints
5. **Contribute** improvements!

---

**Happy building! 🚀**

*Project Voyager - Long-range wireless file transfer made easy*
