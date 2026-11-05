// Bluetooth Serial support for file transfer (owner: system-core-engineer).
// Provides an abstraction layer over USB Serial and BluetoothSerial so the
// LoRa-serial bridge can use either transport.
//
// **Conditional compilation**: BluetoothSerial is only included when VOYAGER_BT_ENABLED
// is defined. This is because BluetoothSerial adds ~500KB to the firmware, which doesn't
// fit in the default partition table. Users who need BT should:
// 1. Add `-D VOYAGER_BT_ENABLED=1` to build_flags
// 2. Use `board_build.partitions = huge_app.csv` (removes OTA but gives 3MB app space)
//
// Without VOYAGER_BT_ENABLED, this module still provides config persistence (so the
// UI can still show BT settings) but the serial abstraction always routes to USB Serial.
//
// Config file: /config/bt_config.json {"bt_name":"Voyager_XXXX","bt_enabled":false}
// Default BT name: "Voyager_" + last 4 hex chars of the MAC address.
#ifndef BT_SERIAL_H
#define BT_SERIAL_H

#include <Arduino.h>

// Active serial mode for the LoRa-serial bridge. Written by WS commands,
// read by send_to_serial() and serial_to_lora(). USB is the default.
// Named BridgeSerialMode to avoid collision with ESP32 core's SerialMode.
enum BridgeSerialMode : uint8_t {
    BRIDGE_SERIAL_USB = 0,
    BRIDGE_SERIAL_BT  = 1
};

extern volatile BridgeSerialMode active_serial_mode;

// BT config structure
struct BtConfig {
    String bt_name;
    bool bt_enabled;
};

// Initialize Bluetooth Serial module. Call once from setup() after LittleFS
// is mounted. This does NOT start BluetoothSerial - it only loads the config.
// BT is started on-demand via start_bt_serial() to save RAM at boot.
void init_bt_serial();

// Start BluetoothSerial on demand (no reboot required). Returns true on success.
// Uses the configured bt_name or default if not set. Saves bt_enabled=true to config.
bool start_bt_serial();

// Stop BluetoothSerial to free resources (~50KB RAM). Returns true if it was running.
// Saves bt_enabled=false to config. Does NOT switch active_serial_mode back to USB.
bool stop_bt_serial();

// Returns true if BluetoothSerial is currently initialized and running.
bool bt_is_running();

// Returns the current BT config (name and enabled state).
BtConfig get_bt_config();

// Returns the default BT name: "Voyager_" + last 4 hex chars of MAC.
String get_default_bt_name();

// Save BT config to /config/bt_config.json. Returns true on success.
bool save_bt_config(const BtConfig& cfg);

// Returns true if BluetoothSerial is initialized and connected.
bool bt_is_connected();

// Returns true if BT support was compiled in (VOYAGER_BT_ENABLED defined).
bool bt_is_available();

// Stream abstraction for the active serial mode. These check active_serial_mode
// and route to either Serial (USB) or SerialBT (Bluetooth).
int active_serial_available();
int active_serial_read();
size_t active_serial_write(const uint8_t* buf, size_t len);
size_t active_serial_write(const char* str);
size_t active_serial_print(const String& str);

// For parsing bt_config.json in core_dispatch.h (Arduino-free, testable).
// Returns default bt_name from MAC string if json is null or invalid.
inline BtConfig bt_parse_config(const char* json, const String& mac_str) {
    BtConfig cfg;
    cfg.bt_enabled = false;
    // Default name: "Voyager_" + last 4 hex chars of MAC (e.g., "Voyager_A1B2")
    if (mac_str.length() >= 5) {
        String suffix = mac_str.substring(mac_str.length() - 5);
        suffix.replace(":", "");
        suffix.toUpperCase();
        cfg.bt_name = "Voyager_" + suffix;
    } else {
        cfg.bt_name = "Voyager_ESP32";
    }
    return cfg;
}

#endif // BT_SERIAL_H
