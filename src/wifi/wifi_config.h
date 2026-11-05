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

// ---------------------------------------------------------------------------
// Scan result aggregation (pure; the channel-by-channel scan merges into this).
// ---------------------------------------------------------------------------
struct WifiScanEntry {
    char ssid[33];
    int8_t rssi;
};

// Adds one scan hit to `list` (count entries, capacity cap) and returns the new count.
// - Hidden networks (empty SSID) and invalid SSIDs are skipped.
// - The same SSID seen again (mesh nodes, extenders, other channels) keeps one entry
//   with the strongest RSSI.
// - When the list is full, a new SSID replaces the weakest entry if it is stronger.
inline size_t wifi_scan_merge(WifiScanEntry* list, size_t count, size_t cap,
                              const char* ssid, int rssi) {
    if (!list || cap == 0 || !wifi_valid_ssid(ssid)) return count;
    if (rssi < -128) rssi = -128;
    if (rssi > 0) rssi = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(list[i].ssid, ssid) == 0) {
            if (rssi > list[i].rssi) list[i].rssi = (int8_t)rssi;
            return count;
        }
    }
    size_t slot = count;
    if (count >= cap) {
        slot = 0;
        for (size_t i = 1; i < count; i++)
            if (list[i].rssi < list[slot].rssi) slot = i;
        if (rssi <= list[slot].rssi) return count;
    } else {
        count++;
    }
    strncpy(list[slot].ssid, ssid, sizeof(list[slot].ssid) - 1);
    list[slot].ssid[sizeof(list[slot].ssid) - 1] = '\0';
    list[slot].rssi = (int8_t)rssi;
    return count;
}

// Strongest first (stable insertion sort; n <= a few dozen).
inline void wifi_scan_sort(WifiScanEntry* list, size_t count) {
    for (size_t i = 1; i < count; i++) {
        WifiScanEntry e = list[i];
        size_t j = i;
        while (j > 0 && list[j - 1].rssi < e.rssi) { list[j] = list[j - 1]; j--; }
        list[j] = e;
    }
}

// ---------------------------------------------------------------------------
// STA BSSID selection (several APs / mesh nodes broadcasting the saved SSID).
// ---------------------------------------------------------------------------
#define WIFI_ROAM_MIN_GAIN_DB 15          // roam only for a big improvement
#define WIFI_ROAM_CHECK_MS    300000u     // at most one background roam check per 5 min
#define WIFI_ROAM_SETTLE_MS   60000u      // never roam in the first minute of a connection

struct WifiApCandidate {
    bool valid = false;
    int8_t rssi = -128;
    uint8_t channel = 0;
    uint8_t bssid[6] = {0, 0, 0, 0, 0, 0};
};

// One scan hit, as the firmware reads it from the driver (list form for tests/callers).
struct WifiApInfo {
    const char* ssid;
    int rssi;
    uint8_t bssid[6];
    uint8_t channel;
};

// Feeds one scan hit into `best`. Only exact (case-sensitive) SSID matches with a
// BSSID and a 2.4 GHz channel (1..14) count; the strongest wins, ties keep the
// first seen. Returns true if `best` changed.
inline bool wifi_consider_ap(WifiApCandidate& best, const char* want_ssid, const char* ssid,
                             int rssi, const uint8_t* bssid, int channel) {
    if (!want_ssid || !want_ssid[0] || !ssid || !bssid) return false;
    if (strcmp(want_ssid, ssid) != 0) return false;
    if (channel < 1 || channel > 14) return false;
    if (rssi < -128) rssi = -128;
    if (rssi > 0) rssi = 0;
    if (best.valid && rssi <= best.rssi) return false;
    best.valid = true;
    best.rssi = (int8_t)rssi;
    best.channel = (uint8_t)channel;
    memcpy(best.bssid, bssid, 6);
    return true;
}

// Strongest-match selection with one BSSID to avoid (the node that just failed us).
// pick() prefers the best non-avoided node and falls back to the avoided one only
// when it is the only node broadcasting the SSID.
struct WifiApSelector {
    WifiApCandidate any, other;
    bool has_avoid = false;
    uint8_t avoid[6] = {0, 0, 0, 0, 0, 0};
    int matches = 0;   // BSSIDs seen with the wanted SSID

    void set_avoid(const uint8_t* b) {
        has_avoid = b != nullptr;
        if (b) memcpy(avoid, b, 6);
    }
    void add(const char* want_ssid, const char* ssid, int rssi, const uint8_t* bssid, int channel) {
        if (!want_ssid || !ssid || strcmp(want_ssid, ssid) != 0) return;
        matches++;
        wifi_consider_ap(any, want_ssid, ssid, rssi, bssid, channel);
        if (!(has_avoid && bssid && memcmp(bssid, avoid, 6) == 0))
            wifi_consider_ap(other, want_ssid, ssid, rssi, bssid, channel);
    }
    const WifiApCandidate& pick() const { return other.valid ? other : any; }
    // True when the only node found is the one we were told to avoid.
    bool only_avoided() const { return has_avoid && any.valid && !other.valid; }
};

// Index of the entry WifiApSelector picks for want_ssid (strongest, skipping `avoid`
// unless it is the only match), or -1 if no entry has that SSID.
inline int wifi_pick_strongest(const WifiApInfo* list, size_t n, const char* want_ssid,
                               const uint8_t* avoid = nullptr) {
    WifiApSelector sel;
    sel.set_avoid(avoid);
    for (size_t i = 0; list && i < n; i++)
        sel.add(want_ssid, list[i].ssid, list[i].rssi, list[i].bssid, list[i].channel);
    const WifiApCandidate& p = sel.pick();
    if (!p.valid) return -1;
    for (size_t i = 0; i < n; i++)
        if (memcmp(list[i].bssid, p.bssid, 6) == 0 && list[i].rssi == p.rssi && list[i].channel == p.channel)
            return (int)i;
    return -1;
}

// What a STA disconnect reason (esp_wifi_types.h WIFI_REASON_*) tells us.
enum WifiDiscKind {
    WIFI_DISC_SELF = 0,   // we left (new config, roam, mode change): not a failure
    WIFI_DISC_BAD_AP,     // that node is weak/far/gone/refusing: try another BSSID next
    WIFI_DISC_AUTH,       // handshake/key failure: most likely the wrong password
    WIFI_DISC_OTHER       // anything else: plain retry with a fresh pick
};

inline WifiDiscKind wifi_classify_reason(int r) {
    switch (r) {
        case 8:    // ASSOC_LEAVE
        case 36:   // STA_LEAVING
            return WIFI_DISC_SELF;
        case 2:    // AUTH_EXPIRE (AP stopped answering auth: weak link / far node)
        case 3:    // AUTH_LEAVE (deauthed: AP reboot or mesh steering)
        case 4:    // ASSOC_EXPIRE
        case 5:    // ASSOC_TOOMANY
        case 6:    // NOT_AUTHED
        case 7:    // NOT_ASSOCED
        case 34:   // MISSING_ACKS
        case 39:   // TIMEOUT
        case 200:  // BEACON_TIMEOUT
        case 201:  // NO_AP_FOUND
        case 202:  // AUTH_FAIL (in ESP-IDF mostly auth frames lost, not the password)
        case 203:  // ASSOC_FAIL
        case 205:  // CONNECTION_FAIL
            return WIFI_DISC_BAD_AP;
        case 14:   // MIC_FAILURE
        case 15:   // 4WAY_HANDSHAKE_TIMEOUT (wrong PSK on WPA2)
        case 23:   // 802_1X_AUTH_FAILED
        case 204:  // HANDSHAKE_TIMEOUT
            return WIFI_DISC_AUTH;
        default:
            return WIFI_DISC_OTHER;
    }
}

inline const char* wifi_reason_name(int r) {
    switch (r) {
        case 1: return "UNSPECIFIED";
        case 2: return "AUTH_EXPIRE";
        case 3: return "AUTH_LEAVE";
        case 4: return "ASSOC_EXPIRE";
        case 5: return "ASSOC_TOOMANY";
        case 6: return "NOT_AUTHED";
        case 7: return "NOT_ASSOCED";
        case 8: return "ASSOC_LEAVE";
        case 14: return "MIC_FAILURE";
        case 15: return "4WAY_HANDSHAKE_TIMEOUT";
        case 23: return "802_1X_AUTH_FAILED";
        case 34: return "MISSING_ACKS";
        case 36: return "STA_LEAVING";
        case 39: return "TIMEOUT";
        case 200: return "BEACON_TIMEOUT";
        case 201: return "NO_AP_FOUND";
        case 202: return "AUTH_FAIL";
        case 203: return "ASSOC_FAIL";
        case 204: return "HANDSHAKE_TIMEOUT";
        case 205: return "CONNECTION_FAIL";
        case 206: return "AP_TSF_RESET";
        case 207: return "ROAMING";
        default: return "OTHER";
    }
}

// Background roam check gate: only on a settled, weak-ish link, rate-limited.
#define WIFI_ROAM_SCAN_BELOW_DBM -60
inline bool wifi_roam_check_due(int cur_rssi, uint32_t ms_connected, uint32_t ms_since_check) {
    if (cur_rssi >= 0 || cur_rssi > WIFI_ROAM_SCAN_BELOW_DBM) return false;
    return ms_connected >= WIFI_ROAM_SETTLE_MS && ms_since_check >= WIFI_ROAM_CHECK_MS;
}

// Roam only to a different BSSID that is at least min_gain_db stronger than the
// current link. cur_rssi 0 means "unknown" (driver not connected): never roam.
inline bool wifi_should_roam(int cur_rssi, const WifiApCandidate& best, const uint8_t* cur_bssid,
                             int min_gain_db = WIFI_ROAM_MIN_GAIN_DB) {
    if (!best.valid || cur_rssi >= 0 || cur_rssi < -128) return false;
    if (cur_bssid && memcmp(cur_bssid, best.bssid, 6) == 0) return false;
    return (int)best.rssi - cur_rssi >= min_gain_db;
}

// "AA:BB:CC:DD:EE:FF" (out must hold 18 bytes).
inline void wifi_format_bssid(const uint8_t* b, char out[18]) {
    static const char hex[] = "0123456789ABCDEF";
    if (!b) { out[0] = '\0'; return; }
    for (int i = 0; i < 6; i++) {
        out[i * 3] = hex[(b[i] >> 4) & 0x0F];
        out[i * 3 + 1] = hex[b[i] & 0x0F];
        out[i * 3 + 2] = i < 5 ? ':' : '\0';
    }
}

// Device hostname for DHCP and mDNS: "project-voyager-54-43-B2-16-F1-5C".
// Letters, digits and '-' only (RFC 952/1123): the old '_' separators are not
// valid in hostnames, so routers and resolvers could drop or refuse the name.
#define WIFI_HOSTNAME_LEN 34   // 16 prefix + 17 MAC + NUL
inline void wifi_format_hostname(const uint8_t mac[6], char out[WIFI_HOSTNAME_LEN]) {
    static const char hex[] = "0123456789ABCDEF";
    static const char prefix[] = "project-voyager-";
    memcpy(out, prefix, 16);
    for (int i = 0; i < 6; i++) {
        out[16 + i * 3] = hex[(mac[i] >> 4) & 0x0F];
        out[16 + i * 3 + 1] = hex[mac[i] & 0x0F];
        out[16 + i * 3 + 2] = i < 5 ? '-' : '\0';
    }
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
