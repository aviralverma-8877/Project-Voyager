// Bluetooth Serial support implementation (owner: system-core-engineer).
// Conditionally compiled: only includes BluetoothSerial when VOYAGER_BT_ENABLED is defined.
// Without BT, this module still provides config persistence but the serial abstraction
// always routes to USB Serial.
#include "bt_serial.h"
#include "support_method.h"
#include <ArduinoJson.h>

#ifdef VOYAGER_BT_ENABLED
#include <BluetoothSerial.h>
// BluetoothSerial instance. Only initialized if bt_enabled is true at boot.
static BluetoothSerial SerialBT;
static bool bt_initialized = false;
#endif

// Global serial mode flag (Contract 6: volatile for cross-task visibility).
volatile BridgeSerialMode active_serial_mode = BRIDGE_SERIAL_USB;

static const char* BT_CONFIG_PATH = "/config/bt_config.json";

String get_default_bt_name()
{
    String mac = WiFi.macAddress();
    // Extract last 4 hex chars (MAC format: "AA:BB:CC:DD:EE:FF")
    if (mac.length() >= 5) {
        String suffix = mac.substring(mac.length() - 5);
        suffix.replace(":", "");
        suffix.toUpperCase();
        return "Voyager_" + suffix;
    }
    return "Voyager_ESP32";
}

BtConfig get_bt_config()
{
    BtConfig cfg;
    cfg.bt_name = get_default_bt_name();
    cfg.bt_enabled = false;

    String content;
    if (!read_file(BT_CONFIG_PATH, content))
        return cfg;

    JsonDocument doc;
    if (deserializeJson(doc, content))
        return cfg;

    if (doc["bt_name"].is<const char*>()) {
        String name = doc["bt_name"].as<const char*>();
        if (name.length() > 0 && name.length() <= 32)
            cfg.bt_name = name;
    }
    if (doc["bt_enabled"].is<bool>())
        cfg.bt_enabled = doc["bt_enabled"].as<bool>();

    return cfg;
}

bool save_bt_config(const BtConfig& cfg)
{
    JsonDocument doc;
    doc["bt_name"] = cfg.bt_name;
    doc["bt_enabled"] = cfg.bt_enabled;
    String content;
    serializeJson(doc, content);
    return write_file_atomic(BT_CONFIG_PATH, content);
}

void init_bt_serial()
{
#ifdef VOYAGER_BT_ENABLED
    BtConfig cfg = get_bt_config();
    serial_print("BT config: name=" + cfg.bt_name + " enabled=" + String(cfg.bt_enabled ? "true" : "false"));
    if (cfg.bt_enabled) {
        // Enabled earlier from the web page: come up at boot so the Android app or a
        // paired PC can connect without anyone opening the page first.
        if (SerialBT.begin(cfg.bt_name)) {
            bt_initialized = true;
            serial_print("BluetoothSerial started at boot");
        } else {
            serial_print("ERROR: BluetoothSerial.begin() failed at boot");
        }
    } else {
        serial_print("Bluetooth is off (enable it on the File Transfer page)");
    }
#else
    serial_print("Bluetooth support not compiled (VOYAGER_BT_ENABLED not defined)");
#endif
}

bool start_bt_serial()
{
#ifdef VOYAGER_BT_ENABLED
    if (bt_initialized) {
        serial_print("BluetoothSerial already running");
        return true;
    }

    BtConfig cfg = get_bt_config();
    serial_print("Starting BluetoothSerial: " + cfg.bt_name);

    if (SerialBT.begin(cfg.bt_name)) {
        bt_initialized = true;
        // Save enabled state so get_bt_config reports correct status
        cfg.bt_enabled = true;
        save_bt_config(cfg);
        serial_print("BluetoothSerial started successfully");
        return true;
    } else {
        serial_print("ERROR: BluetoothSerial.begin() failed");
        return false;
    }
#else
    serial_print("ERROR: Bluetooth not compiled (VOYAGER_BT_ENABLED not defined)");
    return false;
#endif
}

bool stop_bt_serial()
{
#ifdef VOYAGER_BT_ENABLED
    if (!bt_initialized) {
        serial_print("BluetoothSerial not running");
        return false;
    }

    SerialBT.end();
    bt_initialized = false;

    BtConfig cfg = get_bt_config();
    cfg.bt_enabled = false;
    save_bt_config(cfg);

    serial_print("BluetoothSerial stopped");
    return true;
#else
    return false;
#endif
}

bool bt_is_running()
{
#ifdef VOYAGER_BT_ENABLED
    return bt_initialized;
#else
    return false;
#endif
}

bool bt_is_connected()
{
#ifdef VOYAGER_BT_ENABLED
    if (!bt_initialized)
        return false;
    return SerialBT.connected();
#else
    return false;
#endif
}

bool bt_is_available()
{
#ifdef VOYAGER_BT_ENABLED
    return true;   // BT support compiled in
#else
    return false;  // BT support not compiled
#endif
}

void active_serial_autoselect()
{
#ifdef VOYAGER_BT_ENABLED
    static bool was_connected = false;
    if (!bt_initialized || !SerialBT.connected()) {
        active_serial_mode = BRIDGE_SERIAL_USB;
        was_connected = false;
        return;
    }
    if (!was_connected) {                 // a Bluetooth client just connected
        was_connected = true;
        active_serial_mode = BRIDGE_SERIAL_BT;
        return;
    }
    // Both attached: follow the side the host is actually talking on.
    if (active_serial_mode == BRIDGE_SERIAL_BT) {
        if (SerialBT.available() == 0 && Serial.available() > 0)
            active_serial_mode = BRIDGE_SERIAL_USB;
    } else if (SerialBT.available() > 0) {
        active_serial_mode = BRIDGE_SERIAL_BT;
    }
#endif
}

void bt_discard_input()
{
#ifdef VOYAGER_BT_ENABLED
    if (!bt_initialized) return;
    while (SerialBT.available() > 0) SerialBT.read();
#endif
}

// Stream abstraction: route to USB Serial or BluetoothSerial based on active mode.
// When VOYAGER_BT_ENABLED is not defined, always uses USB Serial.

int active_serial_available()
{
#ifdef VOYAGER_BT_ENABLED
    if (active_serial_mode == BRIDGE_SERIAL_BT && bt_initialized)
        return SerialBT.available();
#endif
    return Serial.available();
}

int active_serial_read()
{
#ifdef VOYAGER_BT_ENABLED
    if (active_serial_mode == BRIDGE_SERIAL_BT && bt_initialized)
        return SerialBT.read();
#endif
    return Serial.read();
}

size_t active_serial_write(const uint8_t* buf, size_t len)
{
#ifdef VOYAGER_BT_ENABLED
    if (active_serial_mode == BRIDGE_SERIAL_BT && bt_initialized)
        return SerialBT.write(buf, len);
#endif
    return Serial.write(buf, len);
}

size_t active_serial_write(const char* str)
{
#ifdef VOYAGER_BT_ENABLED
    if (active_serial_mode == BRIDGE_SERIAL_BT && bt_initialized)
        return SerialBT.write((const uint8_t*)str, strlen(str));
#endif
    return Serial.write(str);
}

size_t active_serial_print(const String& str)
{
#ifdef VOYAGER_BT_ENABLED
    if (active_serial_mode == BRIDGE_SERIAL_BT && bt_initialized)
        return SerialBT.print(str);
#endif
    return Serial.print(str);
}
