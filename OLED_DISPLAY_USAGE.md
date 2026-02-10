# OLED Display System Usage Guide

## Overview

The Professional OLED Display system provides a modern, clean interface with status icons, animations, and multiple display modes for the Project Voyager ESP32-LoRa device.

## Features

✅ **Professional Layout**
- Header bar with project name
- Status bar with WiFi, LoRa, and MQTT icons
- Signal strength indicator
- Clean content area

✅ **Multiple Display Modes**
- Splash screen with boot animation
- Status screen with live information
- Connecting screen with progress bar
- Error screen with visual feedback
- Custom message screens

✅ **Status Indicators**
- WiFi: Disconnected, Connecting (animated), AP mode, STA mode
- LoRa: Idle, Transmitting, Receiving, Error
- MQTT: Connected indicator
- Signal strength: 0-4 bars

✅ **Animations**
- Boot animation with expanding circles
- Connecting animation with progress
- Blinking indicators

---

## Quick Start

### 1. Initialize Display (in main.cpp)

```cpp
#include "oled_display.h"

void setup() {
    init_oled(); // Automatically shows boot animation
}
```

### 2. Update WiFi Status

```cpp
#include "oled_display.h"

// When WiFi connects in AP mode
set_wifi_status(WIFI_CONNECTED_AP, "Voyager", "192.168.4.1");
set_signal_strength(3); // 3 out of 4 bars
show_status_screen();

// When WiFi connects in STA mode
set_wifi_status(WIFI_CONNECTED_STA, "MyNetwork", "192.168.1.100");
set_signal_strength(4); // Full signal
show_status_screen();

// When connecting
set_wifi_status(WIFI_CONNECTING);
show_connecting_screen("Connecting to WiFi", 50); // 50% progress
```

### 3. Update LoRa Status

```cpp
// Before transmitting
set_lora_status(LORA_TRANSMITTING);
show_status_screen();

// When receiving
set_lora_status(LORA_RECEIVING);
show_status_screen();

// When idle
set_lora_status(LORA_IDLE);
show_status_screen();

// On error
set_lora_status(LORA_ERROR);
show_status_screen();
```

### 4. Update MQTT Status

```cpp
// When MQTT connects
set_mqtt_status(true);
show_status_screen();

// When MQTT disconnects
set_mqtt_status(false);
show_status_screen();
```

### 5. Show Messages

```cpp
// Success message
show_message_screen("Success", "File transferred successfully");

// Error message
show_error_screen("Connection timeout");

// Custom status message
set_status_message("Ready");
show_status_screen();
```

---

## API Reference

### Display Initialization

```cpp
void init_oled();
```
Initializes the OLED display and shows boot animation.

---

### High-Level Display Functions

#### Show Splash Screen
```cpp
void show_splash_screen();
```
Displays the project name and subtitle with decorative elements.

#### Show Status Screen
```cpp
void show_status_screen();
```
Shows the main status screen with header, status bar, and content area.

#### Show Connecting Screen
```cpp
void show_connecting_screen(const char* message, uint8_t progress);
```
- **message**: Status message (e.g., "Connecting...")
- **progress**: 0-100 percentage

Example:
```cpp
show_connecting_screen("Uploading file", 75);
```

#### Show Error Screen
```cpp
void show_error_screen(const char* error_message);
```
Displays an error icon with message.

Example:
```cpp
show_error_screen("LoRa init failed");
```

#### Show Message Screen
```cpp
void show_message_screen(const char* title, const char* message);
```
Shows a custom message with title.

Example:
```cpp
show_message_screen("Notification", "System rebooting in 5s");
```

---

### Status Update Functions

#### Set WiFi Status
```cpp
void set_wifi_status(WiFiStatus status, const char* ssid = nullptr, const char* ip = nullptr);
```

**WiFiStatus enum:**
- `WIFI_DISCONNECTED`
- `WIFI_CONNECTING`
- `WIFI_CONNECTED_AP`
- `WIFI_CONNECTED_STA`

Example:
```cpp
set_wifi_status(WIFI_CONNECTED_STA, "HomeNetwork", "192.168.1.42");
```

#### Set LoRa Status
```cpp
void set_lora_status(LoRaStatus status);
```

**LoRaStatus enum:**
- `LORA_IDLE`
- `LORA_TRANSMITTING`
- `LORA_RECEIVING`
- `LORA_ERROR`

#### Set MQTT Status
```cpp
void set_mqtt_status(bool connected);
```

#### Set Status Message
```cpp
void set_status_message(const char* message);
```
Updates the status message shown in the content area.

#### Set Signal Strength
```cpp
void set_signal_strength(uint8_t strength); // 0-4
```

---

### Drawing Helper Functions

#### Draw Centered Text
```cpp
void draw_centered_text(const char* text, int16_t y, uint8_t size = 1);
```

#### Draw Wrapped Text
```cpp
void draw_wrapped_text(const char* text, int16_t x, int16_t y, int16_t max_width, uint8_t size = 1);
```

#### Draw Progress Bar
```cpp
void draw_progress_bar(int16_t x, int16_t y, int16_t width, int16_t height, uint8_t progress);
```

---

## Migration from Legacy Display

The new system maintains backward compatibility with the old `display_buffer[]` system.

### Old Method (Still Works)
```cpp
display_buffer[0].msg = "Voyager";
display_buffer[1].msg = "WiFi: Connected";
display_text_oled();
```

### New Method (Recommended)
```cpp
set_wifi_status(WIFI_CONNECTED_AP, nullptr, nullptr);
display_state.line1 = "WiFi: Connected";
show_status_screen();
```

---

## Complete Example: WiFi Connection Flow

```cpp
#include "oled_display.h"

void connect_to_wifi(const char* ssid, const char* password) {
    // Show connecting screen
    set_wifi_status(WIFI_CONNECTING);
    show_connecting_screen("Connecting to WiFi", 0);

    WiFi.begin(ssid, password);

    uint8_t progress = 0;
    uint8_t attempts = 0;

    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
        progress += 5;
        show_connecting_screen("Connecting to WiFi", progress);
        attempts++;
    }

    if (WiFi.isConnected()) {
        // Success!
        IPAddress ip = WiFi.localIP();
        set_wifi_status(WIFI_CONNECTED_STA, ssid, ip.toString().c_str());
        set_signal_strength(4); // Full signal
        set_status_message("Connected");
        show_status_screen();
    } else {
        // Failed
        show_error_screen("WiFi connection failed");
        vTaskDelay(3000 / portTICK_PERIOD_MS);

        // Fallback to AP mode
        set_wifi_status(WIFI_CONNECTED_AP, "Voyager", "192.168.4.1");
        show_status_screen();
    }
}
```

---

## Complete Example: File Transfer Progress

```cpp
void transfer_file_via_lora(const char* filename, size_t file_size) {
    set_lora_status(LORA_TRANSMITTING);
    set_status_message("Sending...");
    show_status_screen();

    size_t bytes_sent = 0;

    while (bytes_sent < file_size) {
        // Send chunk
        send_lora_chunk();
        bytes_sent += CHUNK_SIZE;

        // Update progress
        uint8_t progress = (bytes_sent * 100) / file_size;
        show_connecting_screen("Transferring file", progress);

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }

    // Transfer complete
    set_lora_status(LORA_IDLE);
    set_status_message("Transfer complete");
    show_status_screen();
}
```

---

## Screen Layouts

### Status Screen Layout
```
┌──────────────────────────┐
│ Voyager              [Header]
├──────────────────────────┤
│ Mode: AP             [Content]
│ Voyager
│ 192.168.4.1
│ Ready
│
├──────────────────────────┤
│ [WiFi] [LoRa] [M]  ████ [Status Bar]
└──────────────────────────┘
```

### Connecting Screen Layout
```
┌──────────────────────────┐
│ Voyager              [Header]
├──────────────────────────┤
│
│   Connecting...      [Message]
│
│ ▓▓▓▓▓▓▓▓▓░░░░        [Progress]
│        45%
│
└──────────────────────────┘
```

### Error Screen Layout
```
┌──────────────────────────┐
│
│        ⊗             [Error Icon]
│
│      ERROR
│  Connection timeout  [Message]
│
└──────────────────────────┘
```

---

## Display State Reference

The global `display_state` struct contains:

```cpp
struct DisplayState {
    DisplayMode mode;           // Current screen mode
    WiFiStatus wifi_status;     // WiFi connection state
    LoRaStatus lora_status;     // LoRa activity state
    bool mqtt_connected;        // MQTT connection flag
    String title;               // Screen title
    String line1;               // Content line 1 (SSID)
    String line2;               // Content line 2 (IP)
    String line3;               // Content line 3
    String line4;               // Content line 4
    String status_message;      // Status message
    uint8_t signal_strength;    // 0-4 bars
    unsigned long last_update;  // Timestamp
};
```

You can access it directly:
```cpp
display_state.line1 = "Custom text";
display_state.line2 = "More info";
show_status_screen();
```

---

## Best Practices

1. **Always call `show_*_screen()` after updating state**
   ```cpp
   set_wifi_status(WIFI_CONNECTED_AP, "Voyager", "192.168.4.1");
   show_status_screen(); // Don't forget this!
   ```

2. **Use appropriate screen modes for different scenarios**
   - Boot: `show_splash_screen()`
   - Normal operation: `show_status_screen()`
   - Long operations: `show_connecting_screen()`
   - Errors: `show_error_screen()`

3. **Update LoRa status during transmission**
   ```cpp
   set_lora_status(LORA_TRANSMITTING);
   show_status_screen();
   // ... send data ...
   set_lora_status(LORA_IDLE);
   show_status_screen();
   ```

4. **Keep messages concise** (max 20 characters for single line)

5. **Use signal strength indicator** to show WiFi quality

---

## Troubleshooting

### Display is blank
- Check I2C wiring (SDA, SCL pins)
- Verify `SCREEN_ADDRESS` is correct (0x3C or 0x3D)
- Check `init_oled()` is called in setup

### Icons not showing
- Ensure status is set before calling `show_status_screen()`
- Check enum values are correct

### Text cut off
- Text is automatically truncated at 20 characters
- Use `draw_wrapped_text()` for longer messages

### Display not updating
- Always call `show_*_screen()` after state changes
- Check that `display.display()` is being called

---

## Advanced Customization

### Custom Icons

Add your own icon drawing function:

```cpp
void draw_custom_icon(int16_t x, int16_t y) {
    display.fillCircle(x + 4, y + 4, 3, SSD1306_WHITE);
    display.drawLine(x + 4, y, x + 4, y + 8, SSD1306_WHITE);
}
```

### Custom Animations

Create custom animation loops:

```cpp
void show_custom_animation() {
    for(int i = 0; i < 10; i++) {
        clear_oled_display();
        display.drawCircle(64, 32, 5 + i*2, SSD1306_WHITE);
        display.display();
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}
```

---

## Performance Notes

- Screen updates take ~10-20ms
- Animations should use `vTaskDelay()` for smoothness
- Icons are drawn with simple shapes for speed
- Text wrapping is automatic but may be slow for very long strings

---

**For more information, see the source code:**
- [oled_display.h](src/oled_display.h) - API definitions
- [oled_display.cpp](src/oled_display.cpp) - Implementation
