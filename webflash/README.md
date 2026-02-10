# Project Voyager - Web Flash Tool

This directory contains pre-built firmware binaries for flashing ESP32 devices via the [ESP Web Tools](https://esphome.github.io/esp-web-tools/) web-based flasher.

## Quick Start

### Option 1: Use the Build Scripts (Recommended)

Run the automated build script to compile and prepare all files:

**Windows:**
```bash
build_webflash.bat
```

**Linux/Mac/Windows (Python):**
```bash
python build_webflash.py
```

### Option 2: Manual Build

If you prefer to build manually:

```bash
# Build firmware for version 1
pio run -e esp_lora_v1

# Build firmware for version 2
pio run -e esp_lora_v2

# Build filesystem
pio run -e esp_lora_v1 --target buildfs

# Copy files to webflash directory (see build scripts for exact commands)
```

## Files in This Directory

### Hardware Version 1 (manifest-1.json)
- `firmware-1.bin` - Main application firmware (v1 GPIO mapping)
- `littlefs-1.bin` - LittleFS filesystem with web interface
- `bootloader.bin` - ESP32 bootloader
- `partitions.bin` - Partition table
- `boot_app0.bin` - Boot application

### Hardware Version 2 (manifest-2.json)
- `firmware-2.bin` - Main application firmware (v2 GPIO mapping)
- `littlefs-2.bin` - LittleFS filesystem with web interface
- `bootloader.bin` - ESP32 bootloader (same for both versions)
- `partitions.bin` - Partition table (same for both versions)
- `boot_app0.bin` - Boot application (same for both versions)

## Flash Offsets

| File | Offset (Hex) | Offset (Decimal) | Size |
|------|--------------|------------------|------|
| bootloader.bin | 0x1000 | 4096 | ~26 KB |
| partitions.bin | 0x8000 | 32768 | ~3 KB |
| boot_app0.bin | 0xE000 | 57344 | ~4 KB |
| firmware.bin | 0x10000 | 65536 | ~1.2 MB |
| littlefs.bin | 0x290000 | 2686976 | ~1.5 MB |

## Web Flasher Integration

### Create a Web Flasher Page

Create an `index.html` file with ESP Web Tools:

```html
<!DOCTYPE html>
<html>
<head>
    <title>Project Voyager Web Flasher</title>
    <script type="module" src="https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module"></script>
</head>
<body>
    <h1>Project Voyager - Web Flasher</h1>

    <h2>Hardware Version 1</h2>
    <esp-web-install-button manifest="manifest-1.json">
        <button slot="activate">Flash Version 1</button>
    </esp-web-install-button>

    <h2>Hardware Version 2</h2>
    <esp-web-install-button manifest="manifest-2.json">
        <button slot="activate">Flash Version 2</button>
    </esp-web-install-button>
</body>
</html>
```

### Host the Flasher

Option 1: **GitHub Pages**
1. Commit the webflash directory to your repository
2. Enable GitHub Pages in repository settings
3. Access at: `https://aviralverma-8877.github.io/Project-Voyager/webflash/`

Option 2: **Local Server**
```bash
# Python 3
python -m http.server 8000

# Then open: http://localhost:8000
```

Option 3: **Use ESP Web Tools Directly**
Go to https://esphome.github.io/esp-web-tools/ and upload manifest files

## Flashing via esptool.py

If you prefer command-line flashing:

### Version 1
```bash
esptool.py --after hard_reset write_flash \
  0x1000 bootloader.bin \
  0x8000 partitions.bin \
  0xE000 boot_app0.bin \
  0x10000 firmware-1.bin \
  0x290000 littlefs-1.bin
```

### Version 2
```bash
esptool.py --after hard_reset write_flash \
  0x1000 bootloader.bin \
  0x8000 partitions.bin \
  0xE000 boot_app0.bin \
  0x10000 firmware-2.bin \
  0x290000 littlefs-2.bin
```

## Hardware Version Differences

### Version 1 (esp_lora_v1)
- LoRa SCK: GPIO 18
- LoRa MISO: GPIO 19
- LoRa MOSI: GPIO 23
- LoRa NSS: GPIO 5
- LoRa RST: GPIO 14
- LoRa DIO0: GPIO 2
- LED: GPIO 4
- Button 1: GPIO 32
- Button 2: GPIO 36

### Version 2 (esp_lora_v2)
- LoRa SCK: GPIO 5
- LoRa MISO: GPIO 19
- LoRa MOSI: GPIO 27
- LoRa NSS: GPIO 18
- LoRa RST: GPIO 23
- LoRa DIO0: GPIO 26
- LED: GPIO 2
- Button 1: GPIO 39
- Button 2: GPIO 36

**OLED (Same for both):**
- SCL: GPIO 22
- SDA: GPIO 21

## Troubleshooting

### Build Script Issues

**Error: "PlatformIO not found"**
- Install PlatformIO: https://platformio.org/install
- Add to PATH and restart terminal

**Error: "Build failed"**
- Run `pio run -e esp_lora_v1 -v` for verbose output
- Check for missing dependencies
- Verify `platformio.ini` is correct

### Flashing Issues

**Error: "Failed to connect to ESP32"**
- Hold BOOT button while connecting USB
- Try different USB cable
- Check COM port drivers

**Error: "File not found"**
- Run build script first to generate binaries
- Verify files exist in webflash directory

**Web Flasher Not Working**
- Use Chrome or Edge browser (required for Web Serial API)
- Must be served over HTTPS (except localhost)
- Check browser console for errors

## File Sizes (Approximate)

| File | Size |
|------|------|
| bootloader.bin | 26 KB |
| partitions.bin | 3 KB |
| boot_app0.bin | 4 KB |
| firmware-1.bin | 1.2 MB |
| firmware-2.bin | 1.2 MB |
| littlefs-1.bin | 1.5 MB |
| littlefs-2.bin | 1.5 MB |

**Total per version:** ~2.7 MB

## Manifest File Format

The manifest files follow the [ESP Web Tools format](https://esphome.github.io/esp-web-tools/):

```json
{
  "name": "Project Voyager v1",
  "version": "1.0.0",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "bootloader.bin", "offset": 4096 },
        { "path": "partitions.bin", "offset": 32768 },
        { "path": "boot_app0.bin", "offset": 57344 },
        { "path": "firmware-1.bin", "offset": 65536 },
        { "path": "littlefs-1.bin", "offset": 2686976 }
      ]
    }
  ]
}
```

## Automation

For CI/CD integration, add to your workflow:

**GitHub Actions Example:**
```yaml
- name: Build Firmware
  run: |
    pip install platformio
    python build_webflash.py

- name: Upload Artifacts
  uses: actions/upload-artifact@v3
  with:
    name: firmware
    path: webflash/
```

## Links

- **Repository:** https://github.com/aviralverma-8877/Project-Voyager
- **Issues:** https://github.com/aviralverma-8877/Project-Voyager/issues
- **Author:** Aviral Verma (aviral.verma.8877@gmail.com)
- **ESP Web Tools:** https://esphome.github.io/esp-web-tools/
- **PlatformIO:** https://platformio.org/

---

**Last Updated:** February 2024
