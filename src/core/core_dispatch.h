// Arduino-free core logic (system-core-engineer).
// Pure functions only: ArduinoJson + std::string + plain integers, so the same
// code builds on the ESP32 and in [env:native] (test/test_core).
//
//  - core_parse_request : WS request JSON -> CoreCommand (action + validated args)
//  - core_lora_config_valid : Contract 5 range checks for set_lora_config
//  - core_parse_username / core_parse_lora_serial / core_parse_device_mode :
//    config file parsing with defaults (parse error / missing key -> default)
//  - CoreButton : debounce + hold state machine for BTN1 (SOS) and BTN2 (pages)
#ifndef CORE_DISPATCH_H
#define CORE_DISPATCH_H

#include <ArduinoJson.h>
#include <stdint.h>
#include <string.h>
#include <string>

// ---------------------------------------------------------------- dispatcher

enum class CoreAction : uint8_t {
    Missing,        // no / non-string "request-type"  -> ignore + log
    Unknown,        // unrecognised request-type        -> ignore + log
    Invalid,        // known type, bad arguments        -> ignore + log + alert
    WifiScan,
    ConnectWifi,    // arg1 = ssid, arg2 = pass (may be empty)
    WifiApMode,
    SetApPass,      // arg1 = pass ("" = derived default, else 8-63)
    ResetDevice,
    RestartDevice,
    SetUsername,    // arg1 = username (1-32)
    GetUsername,
    SetLoraConfig,  // arg1 = JSON string, already range-checked
    SetSerialMode,  // flag
    GetSerialMode,
    GetWifiScan,    // resend the last scan result (no new scan)
    // Bluetooth Serial (file transfer support)
    GetBtConfig,    // returns current BT config (name, enabled)
    SetBtMode,      // flag = enable/disable BT, saves config and reboots if enabling
    RebootToBt,     // saves bt_enabled=true and restarts
    // File transfer handshake (LORA_MSG actions)
    FileTxInvite,   // arg1 = fileName, arg2 = fileSize (as string)
    FileTxJoin,     // arg1 = sender_mac, arg2 = mode ("usb" or "bt")
    FileTxStart,    // enables lora_serial bridge
    FileTxEnd,      // disables lora_serial bridge
};

struct CoreCommand {
    CoreAction action = CoreAction::Missing;
    std::string arg1;
    std::string arg2;
    bool flag = false;
    const char* error = "";   // static string, never contains argument values
};

static const size_t CORE_USERNAME_MIN = 1;
static const size_t CORE_USERNAME_MAX = 32;
static const size_t CORE_SSID_MAX = 32;
static const size_t CORE_PASS_MIN = 8;
static const size_t CORE_PASS_MAX = 63;

inline const char* core_action_name(CoreAction a) {
    switch (a) {
        case CoreAction::Missing: return "missing";
        case CoreAction::Unknown: return "unknown";
        case CoreAction::Invalid: return "invalid";
        case CoreAction::WifiScan: return "wifi_ssid_scan";
        case CoreAction::ConnectWifi: return "connect_wifi";
        case CoreAction::WifiApMode: return "wifi_ap_mode";
        case CoreAction::SetApPass: return "set_ap_pass";
        case CoreAction::ResetDevice: return "reset_device";
        case CoreAction::RestartDevice: return "restart_device";
        case CoreAction::SetUsername: return "set_username";
        case CoreAction::GetUsername: return "get_username";
        case CoreAction::SetLoraConfig: return "set_lora_config";
        case CoreAction::SetSerialMode: return "set_serial_mode";
        case CoreAction::GetSerialMode: return "get_serial_mode";
        case CoreAction::GetWifiScan: return "get_wifi_scan";
        case CoreAction::GetBtConfig: return "get_bt_config";
        case CoreAction::SetBtMode: return "set_bt_mode";
        case CoreAction::RebootToBt: return "reboot_to_bt";
        case CoreAction::FileTxInvite: return "file_tx_invite";
        case CoreAction::FileTxJoin: return "file_tx_join";
        case CoreAction::FileTxStart: return "file_tx_start";
        case CoreAction::FileTxEnd: return "file_tx_end";
    }
    return "?";
}

// True if every byte is printable (no control characters, which would
// corrupt the OLED, the JSON files or the log).
inline bool core_printable(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return false;
    return true;
}

// Contract 5 range checks for a LoRa config object.
inline bool core_lora_config_valid(JsonObjectConst o) {
    if (o.isNull()) return false;
    static const char* keys[] = {"freq", "TxPower", "SpreadingFactor",
                                 "SignalBandwidth", "CodingRate4", "SyncWord"};
    for (const char* k : keys)
        if (!o[k].is<long>()) return false;   // integer required (rejects strings/floats/bools)
    long freq = o["freq"].as<long>();
    long tx = o["TxPower"].as<long>();
    long sf = o["SpreadingFactor"].as<long>();
    long bw = o["SignalBandwidth"].as<long>();
    long cr = o["CodingRate4"].as<long>();
    long sw = o["SyncWord"].as<long>();
    if (freq < 410000000L || freq > 525000000L) return false;
    if (tx < 2 || tx > 20) return false;
    if (sf < 6 || sf > 12) return false;
    if (cr < 5 || cr > 8) return false;
    if (sw < 0 || sw > 255) return false;
    static const long bws[] = {7800, 10400, 15600, 20800, 31250,
                               41700, 62500, 125000, 250000, 500000};
    for (long b : bws)
        if (b == bw) return true;
    return false;
}

inline bool core_lora_config_string_valid(const std::string& json) {
    JsonDocument cfg;
    if (deserializeJson(cfg, json)) return false;
    if (!cfg.is<JsonObjectConst>()) return false;
    return core_lora_config_valid(cfg.as<JsonObjectConst>());
}

inline CoreCommand core_invalid(const char* why) {
    CoreCommand c;
    c.action = CoreAction::Invalid;
    c.error = why;
    return c;
}

// Request JSON -> action + validated arguments. Never dereferences a missing key.
inline CoreCommand core_parse_request(JsonVariantConst doc) {
    CoreCommand c;
    JsonVariantConst rt = doc["request-type"];
    if (!rt.is<const char*>()) {
        c.action = CoreAction::Missing;
        c.error = "missing request-type";
        return c;
    }
    const char* t = rt.as<const char*>();

    if (strcmp(t, "wifi_ssid_scan") == 0) { c.action = CoreAction::WifiScan; return c; }
    if (strcmp(t, "wifi_ap_mode") == 0)   { c.action = CoreAction::WifiApMode; return c; }
    if (strcmp(t, "reset_device") == 0)   { c.action = CoreAction::ResetDevice; return c; }
    if (strcmp(t, "restart_device") == 0) { c.action = CoreAction::RestartDevice; return c; }
    if (strcmp(t, "get_username") == 0)   { c.action = CoreAction::GetUsername; return c; }
    if (strcmp(t, "get_serial_mode") == 0){ c.action = CoreAction::GetSerialMode; return c; }
    if (strcmp(t, "get_wifi_scan") == 0)  { c.action = CoreAction::GetWifiScan; return c; }

    if (strcmp(t, "connect_wifi") == 0) {
        JsonVariantConst s = doc["wifi_ssid"];
        JsonVariantConst p = doc["wifi_pass"];
        if (!s.is<const char*>()) return core_invalid("wifi_ssid missing");
        std::string ssid = s.as<const char*>();
        if (ssid.empty() || ssid.size() > CORE_SSID_MAX) return core_invalid("wifi_ssid length");
        std::string pass;
        if (!p.isNull()) {
            if (!p.is<const char*>()) return core_invalid("wifi_pass type");
            pass = p.as<const char*>();
        }
        // Empty = open network; otherwise WPA/WPA2 passphrase rules.
        if (!pass.empty() && (pass.size() < CORE_PASS_MIN || pass.size() > CORE_PASS_MAX))
            return core_invalid("wifi_pass length");
        c.action = CoreAction::ConnectWifi;
        c.arg1 = ssid;
        c.arg2 = pass;
        return c;
    }
    if (strcmp(t, "set_ap_pass") == 0) {
        JsonVariantConst v = doc["val"];
        if (!v.is<const char*>()) return core_invalid("val missing");
        std::string pass = v.as<const char*>();
        // "" = reset to the derived default (Contract 5).
        if (!pass.empty() && (pass.size() < CORE_PASS_MIN || pass.size() > CORE_PASS_MAX))
            return core_invalid("ap_pass length");
        if (!core_printable(pass)) return core_invalid("ap_pass chars");
        c.action = CoreAction::SetApPass;
        c.arg1 = pass;
        return c;
    }
    if (strcmp(t, "set_username") == 0) {
        JsonVariantConst v = doc["val"];
        if (!v.is<const char*>()) return core_invalid("val missing");
        std::string u = v.as<const char*>();
        if (u.size() < CORE_USERNAME_MIN || u.size() > CORE_USERNAME_MAX)
            return core_invalid("username length");
        if (!core_printable(u)) return core_invalid("username chars");
        c.action = CoreAction::SetUsername;
        c.arg1 = u;
        return c;
    }
    if (strcmp(t, "set_lora_config") == 0) {
        JsonVariantConst v = doc["val"];
        if (!v.is<const char*>()) return core_invalid("val missing");
        std::string cfg = v.as<const char*>();
        if (!core_lora_config_string_valid(cfg)) return core_invalid("lora config out of range");
        c.action = CoreAction::SetLoraConfig;
        c.arg1 = cfg;
        return c;
    }
    if (strcmp(t, "set_serial_mode") == 0) {
        JsonVariantConst v = doc["value"];
        if (!v.is<bool>()) return core_invalid("value must be bool");
        c.action = CoreAction::SetSerialMode;
        c.flag = v.as<bool>();
        return c;
    }
    // Bluetooth Serial commands
    if (strcmp(t, "get_bt_config") == 0)  { c.action = CoreAction::GetBtConfig; return c; }
    if (strcmp(t, "reboot_to_bt") == 0)   { c.action = CoreAction::RebootToBt; return c; }
    if (strcmp(t, "set_bt_mode") == 0) {
        JsonVariantConst v = doc["value"];
        if (!v.is<bool>()) return core_invalid("value must be bool");
        c.action = CoreAction::SetBtMode;
        c.flag = v.as<bool>();
        return c;
    }
    // File transfer handshake commands
    if (strcmp(t, "file_tx_invite") == 0) {
        JsonVariantConst fn = doc["fileName"];
        JsonVariantConst fs = doc["fileSize"];
        if (!fn.is<const char*>()) return core_invalid("fileName missing");
        if (!fs.is<long>() && !fs.is<const char*>()) return core_invalid("fileSize missing");
        c.action = CoreAction::FileTxInvite;
        c.arg1 = fn.as<const char*>();
        // Accept fileSize as number or string
        if (fs.is<long>())
            c.arg2 = std::to_string(fs.as<long>());
        else
            c.arg2 = fs.as<const char*>();
        return c;
    }
    if (strcmp(t, "file_tx_join") == 0) {
        JsonVariantConst sm = doc["sender_mac"];
        JsonVariantConst m = doc["mode"];
        if (!sm.is<const char*>()) return core_invalid("sender_mac missing");
        if (!m.is<const char*>()) return core_invalid("mode missing");
        std::string mode = m.as<const char*>();
        if (mode != "usb" && mode != "bt") return core_invalid("mode must be usb or bt");
        c.action = CoreAction::FileTxJoin;
        c.arg1 = sm.as<const char*>();
        c.arg2 = mode;
        return c;
    }
    if (strcmp(t, "file_tx_start") == 0) { c.action = CoreAction::FileTxStart; return c; }
    if (strcmp(t, "file_tx_end") == 0)   { c.action = CoreAction::FileTxEnd; return c; }

    c.action = CoreAction::Unknown;   // includes the removed "send_akn"
    c.error = "unknown request-type";
    return c;
}

// ------------------------------------------------------------ config parsing

// user_data.json {username}. Missing file (nullptr), parse error, missing /
// non-string / empty / too long / control chars -> the MAC address.
inline std::string core_parse_username(const char* json, const std::string& mac) {
    if (!json) return mac;
    JsonDocument doc;
    if (deserializeJson(doc, json)) return mac;
    JsonVariantConst u = doc["username"];
    if (!u.is<const char*>()) return mac;
    std::string s = u.as<const char*>();
    if (s.size() < CORE_USERNAME_MIN || s.size() > CORE_USERNAME_MAX || !core_printable(s))
        return mac;
    return s;
}

// lora_serial.json {lora_serial}. Anything but a JSON true -> false.
inline bool core_parse_lora_serial(const char* json) {
    if (!json) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    JsonVariantConst v = doc["lora_serial"];
    return v.is<bool>() && v.as<bool>();
}

// device_config.json {device_mode}. Default "WiFi".
inline std::string core_parse_device_mode(const char* json) {
    if (!json) return "WiFi";
    JsonDocument doc;
    if (deserializeJson(doc, json)) return "WiFi";
    JsonVariantConst v = doc["device_mode"];
    if (!v.is<const char*>()) return "WiFi";
    std::string s = v.as<const char*>();
    return s.empty() ? std::string("WiFi") : s;
}

// --------------------------------------------------------- button machine

static const uint32_t CORE_BTN_DEBOUNCE_MS = 30;
static const uint32_t CORE_SOS_HOLD_MS = 2000;
static const uint32_t CORE_BTN2_LONG_MS = 1000;

enum class BtnEvent : uint8_t {
    None,
    Pressed,     // debounced press edge
    Progress,    // still held, below threshold; pct valid
    HoldFired,   // threshold reached (fires once per press)
    ShortRelease,// released before the threshold (BTN1: cancel, BTN2: short press)
    LongRelease, // released after HoldFired
};

struct BtnOutput {
    BtnEvent ev = BtnEvent::None;
    uint8_t pct = 0;   // 0-100 while held
};

// Feed the raw level every poll. Time arithmetic is wrap-safe (uint32_t).
struct CoreButton {
    uint32_t hold_ms;
    bool raw = false;            // last raw sample
    uint32_t raw_since = 0;      // when raw last changed
    bool stable = false;         // debounced level
    uint32_t press_start = 0;
    bool fired = false;
    uint8_t last_pct = 255;

    explicit CoreButton(uint32_t hold) : hold_ms(hold) {}

    BtnOutput update(bool pressed, uint32_t now) {
        BtnOutput out;
        if (pressed != raw) {
            raw = pressed;
            raw_since = now;
        }
        if (raw != stable && (uint32_t)(now - raw_since) >= CORE_BTN_DEBOUNCE_MS) {
            stable = raw;
            if (stable) {
                press_start = now;
                fired = false;
                last_pct = 0;
                out.ev = BtnEvent::Pressed;
                out.pct = 0;
                return out;
            }
            out.ev = fired ? BtnEvent::LongRelease : BtnEvent::ShortRelease;
            fired = false;
            last_pct = 255;
            return out;
        }
        if (stable && !fired) {
            uint32_t held = now - press_start;
            if (held >= hold_ms) {
                fired = true;
                out.ev = BtnEvent::HoldFired;
                out.pct = 100;
                return out;
            }
            uint8_t pct = (uint8_t)((uint64_t)held * 100u / hold_ms);
            if (pct != last_pct) {
                last_pct = pct;
                out.ev = BtnEvent::Progress;
                out.pct = pct;
            }
        }
        return out;
    }
};

#endif // CORE_DISPATCH_H
