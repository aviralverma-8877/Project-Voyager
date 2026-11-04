// Arduino-free WiFi config helpers (Contract 5). Owner: wifi-network-engineer.
// Pure logic only: JSON parse with defaults/validation, validators, backoff,
// derived AP password. Unit-tested natively in test/test_wifi/.
#ifndef VOYAGER_WIFI_CONFIG_H
#define VOYAGER_WIFI_CONFIG_H

#include <ArduinoJson.h>
#include <cstdint>
#include <cstring>
#include <string>

#define WIFI_DEFAULT_AP_SSID "Voyager"
#define WIFI_BACKOFF_MIN_MS 5000u
#define WIFI_BACKOFF_MAX_MS 60000u
#define WIFI_STA_CONNECT_TIMEOUT_S 20

enum WifiCfgMode { WIFI_CFG_AP = 0, WIFI_CFG_STA = 1 };

struct WifiConfig {
    WifiCfgMode mode = WIFI_CFG_AP;
    std::string ssid = WIFI_DEFAULT_AP_SSID;
    std::string pass;     // STA password (empty = open network)
    std::string ap_pass;  // empty = derived default (wifi_default_ap_pass)
};

enum WifiParseResult {
    WIFI_PARSE_OK = 0,       // every field present and valid
    WIFI_PARSE_DEFAULTED,    // valid JSON object, but some field was missing/invalid and defaulted
    WIFI_PARSE_ERROR         // not a JSON object: caller should try the next source
};

// Printable ASCII only (0x20..0x7E) for passphrases.
inline bool wifi_printable_ascii(const char* s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

// SSID: 1..32 bytes, no control characters (UTF-8 bytes >= 0x80 allowed).
inline bool wifi_valid_ssid(const char* s) {
    if (!s) return false;
    size_t len = strlen(s);
    if (len < 1 || len > 32) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

// STA password: empty (open network) or 8..63 printable ASCII.
inline bool wifi_valid_sta_pass(const char* s) {
    if (!s) return false;
    size_t len = strlen(s);
    if (len == 0) return true;
    return len >= 8 && len <= 63 && wifi_printable_ascii(s, len);
}

// AP password (WPA2): 8..63 printable ASCII.
inline bool wifi_valid_ap_pass(const char* s) {
    if (!s) return false;
    size_t len = strlen(s);
    return len >= 8 && len <= 63 && wifi_printable_ascii(s, len);
}

// Value accepted by wifi_set_ap_pass: "" (use derived default) or a valid AP password.
inline bool wifi_valid_ap_pass_setting(const char* s) {
    if (!s) return false;
    return s[0] == '\0' || wifi_valid_ap_pass(s);
}

// Derived default AP password: uppercase hex of the lower 4 MAC bytes (mac[2..5]).
inline std::string wifi_derive_ap_pass(const uint8_t mac[6]) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(8);
    for (int i = 2; i < 6; i++) {
        out.push_back(hex[(mac[i] >> 4) & 0x0F]);
        out.push_back(hex[mac[i] & 0x0F]);
    }
    return out;
}

// Reconnect backoff: doubles each retry (5s -> 10s -> 20s -> 40s -> 60s), capped at 60s.
inline uint32_t wifi_next_backoff(uint32_t current_ms) {
    if (current_ms < WIFI_BACKOFF_MIN_MS) return WIFI_BACKOFF_MIN_MS;        // first retry: 5s
    if (current_ms >= WIFI_BACKOFF_MAX_MS / 2) return WIFI_BACKOFF_MAX_MS;   // cap at 60s
    return current_ms * 2;                                                    // double each retry
}

// Parse a Contract 5 wifi config. Never throws, never crashes.
// - Not a JSON object -> WIFI_PARSE_ERROR, `out` = compiled defaults.
// - wifi_function missing/unknown -> AP.
// - STA with invalid SSID or invalid STA password -> AP with default SSID
//   (nothing is written back; the saved file is untouched).
// - AP with invalid SSID -> default SSID.
// - ap_pass missing/invalid -> "" (derived default).
inline WifiParseResult wifi_parse_config(const char* json, size_t len, WifiConfig& out) {
    out = WifiConfig();
    if (!json || len == 0) return WIFI_PARSE_ERROR;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, len);
    if (err || !doc.is<JsonObject>()) return WIFI_PARSE_ERROR;

    bool defaulted = false;
    const char* fn = doc["wifi_function"] | "";
    const char* ssid = doc["wifi_ssid"].is<const char*>() ? doc["wifi_ssid"].as<const char*>() : nullptr;
    const char* pass = doc["wifi_pass"].is<const char*>() ? doc["wifi_pass"].as<const char*>() : nullptr;
    const char* ap_pass = doc["ap_pass"].is<const char*>() ? doc["ap_pass"].as<const char*>() : nullptr;

    if (strcmp(fn, "STA") == 0) {
        if (wifi_valid_ssid(ssid) && wifi_valid_sta_pass(pass ? pass : "")) {
            out.mode = WIFI_CFG_STA;
            out.ssid = ssid;
            out.pass = pass ? pass : "";
            if (!pass) defaulted = true;
        } else {
            out.mode = WIFI_CFG_AP;
            out.ssid = WIFI_DEFAULT_AP_SSID;
            defaulted = true;
        }
    } else {
        if (strcmp(fn, "AP") != 0) defaulted = true;
        out.mode = WIFI_CFG_AP;
        if (wifi_valid_ssid(ssid)) {
            out.ssid = ssid;
        } else {
            out.ssid = WIFI_DEFAULT_AP_SSID;
            defaulted = true;
        }
        if (pass && wifi_valid_sta_pass(pass)) out.pass = pass;
    }

    if (ap_pass && wifi_valid_ap_pass(ap_pass)) {
        out.ap_pass = ap_pass;
    } else {
        out.ap_pass = "";
        if (!ap_pass || ap_pass[0] != '\0') defaulted = true;
    }
    return defaulted ? WIFI_PARSE_DEFAULTED : WIFI_PARSE_OK;
}

inline WifiParseResult wifi_parse_config(const std::string& json, WifiConfig& out) {
    return wifi_parse_config(json.data(), json.size(), out);
}

// Serialize to Contract 5 JSON (compact). Contains secrets: never log the result.
template <typename TString>
inline void wifi_serialize_config(const WifiConfig& c, TString& out) {
    JsonDocument doc;
    doc["wifi_function"] = c.mode == WIFI_CFG_STA ? "STA" : "AP";
    doc["wifi_ssid"] = c.ssid.c_str();
    doc["wifi_pass"] = c.pass.c_str();
    doc["ap_pass"] = c.ap_pass.c_str();
    serializeJson(doc, out);
}

#endif
