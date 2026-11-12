// Bluetooth Serial support for file transfer (owner: system-core-engineer).
// Provides an abstraction layer over USB Serial and BluetoothSerial so the
// LoRa-serial bridge can use either transport.
//
// **Conditional compilation**: BluetoothSerial is only included when VOYAGER_BT_ENABLED
// is defined (platformio.ini defines it for every board). It adds about 760 KB to the
// firmware, which is why the project uses partitions_voyager.csv (1.9 MB app slots).
//
// Without VOYAGER_BT_ENABLED, this module still provides config persistence (so the
// UI can still show BT settings) but the serial abstraction always routes to USB Serial.
//
// Which transport the bridge uses is automatic (active_serial_autoselect): Bluetooth
// while a Bluetooth client is connected, USB otherwise, and whichever of the two the
// host last sent data on when both are attached. The LoRa network (Voyager Link SLIP)
// and Improv WiFi setup stay on USB only.
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
// is mounted. Starts BluetoothSerial if bt_enabled was saved as true (so a phone
// can connect without the web page); otherwise it stays off to save RAM until
// start_bt_serial() is called.
void init_bt_serial();

// Picks the bridge transport (see the header comment). Call often from the bridge
// task; cheap. Only the bridge task may call it.
void active_serial_autoselect();

// Throws away pending Bluetooth input (used while the bridge is off, so stale bytes
// are not sent over the radio when it turns on).
void bt_discard_input();

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
