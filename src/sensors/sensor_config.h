// Arduino-free sensor logic: config validation, pin rules and the "sensor"
// LORA_MSG payload. Header-only so the firmware (sensor_support.cpp) and the
// native Unity tests (test/test_sensors) share the same code.
// Depends only on the C/C++ standard library and ArduinoJson.
#ifndef SENSOR_CONFIG_H
#define SENSOR_CONFIG_H

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>

namespace sensor_cfg {

constexpr size_t MAX_COUNT        = 4;
constexpr size_t NAME_LEN_MAX           = 12;      // [A-Za-z0-9_-], so it never needs JSON escaping
constexpr long   INTERVAL_MIN_S     = 2;       // DHT22 cannot be sampled faster
constexpr long   INTERVAL_MAX_S     = 86400;
constexpr long   INTERVAL_DEFAULT_S = 30;
constexpr size_t CONFIG_MAX_BYTES   = 1024;

// The numeric value goes over the air: never renumber, only append.
enum Type : uint8_t {
    T_NONE    = 0,
    T_DHT11   = 1,   // values: temperature C, humidity %
    T_DHT22   = 2,   // values: temperature C, humidity %
    T_DS18B20 = 3,   // values: temperature C
    T_HCSR04  = 4,   // values: distance cm      (pin = trigger, pin2 = echo)
    T_ANALOG  = 5,   // values: raw ADC 0-4095   (LDR, soil moisture, MQ gas, ...)
    T_DIGITAL = 6,   // values: 0 / 1            (PIR, reed switch, rain, ...)
};

struct Sensor {
    uint8_t type = T_NONE;
    int8_t  pin  = -1;
    int8_t  pin2 = -1;                 // HC-SR04 echo only
    char    name[NAME_LEN_MAX + 1] = {0};
};

struct Config {
    bool    enabled  = false;
    long    interval = INTERVAL_DEFAULT_S;   // seconds between broadcasts
    bool    ack      = false;                // wait for the peer's acknowledgement
    uint8_t count    = 0;
    Sensor  sensors[MAX_COUNT];
};

inline const char* type_name(uint8_t t) {
    switch (t) {
        case T_DHT11:   return "dht11";
        case T_DHT22:   return "dht22";
        case T_DS18B20: return "ds18b20";
        case T_HCSR04:  return "hcsr04";
        case T_ANALOG:  return "analog";
        case T_DIGITAL: return "digital";
    }
    return "";
}

inline uint8_t type_from_name(const char* s) {
    if (s == nullptr) return T_NONE;
    for (uint8_t t = T_DHT11; t <= T_DIGITAL; t++)
        if (strcmp(s, type_name(t)) == 0) return t;
    return T_NONE;
}

inline uint8_t value_count(uint8_t t) {
    return (t == T_DHT11 || t == T_DHT22) ? 2 : (t >= T_DS18B20 && t <= T_DIGITAL) ? 1 : 0;
}

// ---- pin rules ---------------------------------------------------------------
// Candidates leave out the flash pins (6-11), the UART (1, 3), the strapping pins
// 0, 2, 5 and 12, and every ADC2 pin's analog function (ADC2 is unusable while
// WiFi is on). GPIO 15 is a strapping pin too, but it only silences the boot
// log when held low at reset, so it is offered (it is the radio's DIO1 on v1). `used` is the board's own pin map (radio, buttons, LED, OLED).

inline bool pin_is_io_candidate(int pin) {
    static const int8_t k[] = {4, 13, 14, 15, 16, 17, 25, 26, 27, 32, 33};
    for (int8_t p : k) if (p == pin) return true;
    return false;
}
inline bool pin_is_input_only(int pin) { return pin == 34 || pin == 35 || pin == 36 || pin == 39; }
inline bool pin_is_adc1(int pin) { return pin == 32 || pin == 33 || pin_is_input_only(pin); }

inline bool pin_in_use(int pin, const int8_t* used, size_t used_n) {
    for (size_t i = 0; i < used_n; i++) if (used[i] == pin) return true;
    return false;
}
// Can drive and read (DHT, DS18B20, HC-SR04 trigger).
inline bool pin_ok_io(int pin, const int8_t* used, size_t n) {
    return pin_is_io_candidate(pin) && !pin_in_use(pin, used, n);
}
// Can read (digital input, HC-SR04 echo).
inline bool pin_ok_input(int pin, const int8_t* used, size_t n) {
    return (pin_is_io_candidate(pin) || pin_is_input_only(pin)) && !pin_in_use(pin, used, n);
}
// Can do analogRead with WiFi on.
inline bool pin_ok_adc(int pin, const int8_t* used, size_t n) {
    return pin_is_adc1(pin) && pin_ok_input(pin, used, n);
}

inline bool name_valid(const char* s) {
    if (s == nullptr) return false;
    size_t n = strlen(s);
    if (n < 1 || n > NAME_LEN_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// ---- config JSON -------------------------------------------------------------
// {"enabled":bool,"interval":seconds,"ack":bool,
//  "sensors":[{"type":"dht22","name":"room","pin":13,"pin2":-1}, ...]}
// Returns false and sets *err (static string) on the first problem; `out` is
// only written on success.
inline bool config_from_json(JsonVariantConst v, Config& out, const int8_t* used, size_t used_n,
                             const char** err = nullptr) {
    auto fail = [&](const char* why) { if (err) *err = why; return false; };
    if (!v.is<JsonObjectConst>()) return fail("not an object");
    Config c;
    if (!v["enabled"].is<bool>()) return fail("enabled must be true or false");
    c.enabled = v["enabled"].as<bool>();
    if (!v["ack"].is<bool>()) return fail("ack must be true or false");
    c.ack = v["ack"].as<bool>();
    if (!v["interval"].is<long>()) return fail("interval must be a whole number of seconds");
    c.interval = v["interval"].as<long>();
    if (c.interval < INTERVAL_MIN_S || c.interval > INTERVAL_MAX_S) return fail("interval out of range");

    JsonVariantConst list = v["sensors"];
    if (!list.is<JsonArrayConst>()) return fail("sensors must be a list");
    JsonArrayConst arr = list.as<JsonArrayConst>();
    if (arr.size() > MAX_COUNT) return fail("too many sensors");

    int8_t taken[MAX_COUNT * 2];
    size_t taken_n = 0;
    auto claim = [&](int pin) {
        if (pin_in_use(pin, taken, taken_n)) return false;
        taken[taken_n++] = (int8_t)pin;
        return true;
    };

    for (JsonVariantConst e : arr) {
        if (!e.is<JsonObjectConst>()) return fail("sensor entry is not an object");
        Sensor s;
        s.type = type_from_name(e["type"] | (const char*)nullptr);
        if (s.type == T_NONE) return fail("unknown sensor type");
        const char* name = e["name"] | (const char*)nullptr;
        if (!name_valid(name)) return fail("sensor name must be 1-12 of A-Z a-z 0-9 _ -");
        for (uint8_t i = 0; i < c.count; i++)
            if (strcmp(c.sensors[i].name, name) == 0) return fail("sensor names must be different");
        strcpy(s.name, name);

        if (!e["pin"].is<long>()) return fail("sensor pin missing");
        long pin = e["pin"].as<long>();
        bool pin_ok = s.type == T_ANALOG    ? pin_ok_adc((int)pin, used, used_n)
                      : s.type == T_DIGITAL ? pin_ok_input((int)pin, used, used_n)
                                            : pin_ok_io((int)pin, used, used_n);
        if (pin < 0 || pin > 39 || !pin_ok) return fail("pin not usable for this sensor type");
        if (!claim((int)pin)) return fail("a pin is used twice");
        s.pin = (int8_t)pin;

        if (s.type == T_HCSR04) {
            if (!e["pin2"].is<long>()) return fail("echo pin missing");
            long pin2 = e["pin2"].as<long>();
            if (pin2 < 0 || pin2 > 39 || !pin_ok_input((int)pin2, used, used_n))
                return fail("echo pin not usable");
            if (!claim((int)pin2)) return fail("a pin is used twice");
            s.pin2 = (int8_t)pin2;
        }
        c.sensors[c.count++] = s;
    }
    if (c.enabled && c.count == 0) return fail("add at least one sensor before enabling");
    out = c;
    return true;
}

inline bool config_from_text(const char* text, Config& out, const int8_t* used, size_t used_n,
                             const char** err = nullptr) {
    if (text == nullptr || strlen(text) > CONFIG_MAX_BYTES) { if (err) *err = "bad json"; return false; }
    JsonDocument doc;
    if (deserializeJson(doc, text)) { if (err) *err = "bad json"; return false; }
    return config_from_json(doc.as<JsonVariantConst>(), out, used, used_n, err);
}

inline void config_to_json(const Config& c, JsonObject o) {
    o["enabled"] = c.enabled;
    o["interval"] = c.interval;
    o["ack"] = c.ack;
    JsonArray arr = o["sensors"].to<JsonArray>();
    for (uint8_t i = 0; i < c.count; i++) {
        JsonObject s = arr.add<JsonObject>();
        s["type"] = type_name(c.sensors[i].type);
        s["name"] = (const char*)c.sensors[i].name;
        s["pin"] = c.sensors[i].pin;
        s["pin2"] = c.sensors[i].pin2;
    }
}

inline std::string config_to_text(const Config& c) {
    JsonDocument doc;
    config_to_json(c, doc.to<JsonObject>());
    std::string out;
    serializeJson(doc, out);
    return out;
}

// ---- readings and the over-the-air payload -------------------------------------
// LORA_MSG inner JSON:
//   {"pack_type":"sensor","data":[["<name>",<type>,<v1>[,<v2>]], ...]}
// A failed read sends null for each value. Built by hand (names need no
// escaping) so the number format is fixed: one decimal, integers for raw values.

struct Reading {
    char    name[NAME_LEN_MAX + 1] = {0};
    uint8_t type = T_NONE;
    bool    ok = false;
    float   v[2] = {0, 0};
};

inline std::string reading_json(const Reading& r) {
    std::string s = "[\"";
    s += r.name;
    s += "\",";
    char buf[24];
    snprintf(buf, sizeof buf, "%u", (unsigned)r.type);
    s += buf;
    uint8_t n = value_count(r.type);
    bool integer = r.type == T_ANALOG || r.type == T_DIGITAL;
    for (uint8_t i = 0; i < n; i++) {
        if (!r.ok) { s += ",null"; continue; }
        if (integer) snprintf(buf, sizeof buf, ",%ld", (long)r.v[i]);
        else         snprintf(buf, sizeof buf, ",%.1f", (double)r.v[i]);
        s += buf;
    }
    s += "]";
    return s;
}

inline std::string inner_json(const Reading* r, size_t n) {
    std::string s = "{\"pack_type\":\"sensor\",\"data\":[";
    for (size_t i = 0; i < n; i++) {
        if (i) s += ",";
        s += reading_json(r[i]);
    }
    s += "]}";
    return s;
}

// Size of the LORA_MSG payload {"name","mac","data":inner} once serialized.
inline size_t wrapped_len(const char* name, const char* mac, const std::string& inner) {
    JsonDocument doc;
    doc["name"] = name ? name : "";
    doc["mac"] = mac ? mac : "";
    doc["data"] = inner.c_str();
    return measureJson(doc);
}

// Packs as many readings as fit in one packet, starting at `start`. Returns how
// many were taken (0 = not even one fits) and writes the inner JSON for them.
inline size_t pack(const Reading* r, size_t n, size_t start, const char* name, const char* mac,
                   size_t max_payload, std::string& inner_out) {
    size_t taken = 0;
    inner_out.clear();
    while (start + taken < n) {
        std::string candidate = inner_json(r + start, taken + 1);
        if (wrapped_len(name, mac, candidate) > max_payload) break;
        inner_out = candidate;
        taken++;
    }
    return taken;
}

}  // namespace sensor_cfg

#endif  // SENSOR_CONFIG_H
