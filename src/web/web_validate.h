// Pure request-validation helpers for the web API (web-api-engineer).
// No Arduino / ESP-IDF dependencies: built natively by test/test_web_api.
#ifndef WEB_VALIDATE_H
#define WEB_VALIDATE_H

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace wv {

static const size_t WS_MAX_MESSAGE = 2048;   // Contract 3
static const size_t PASSWORD_MIN_LEN = 8;            // Contract 2
static const size_t PASSWORD_MAX_LEN = 63;
static const size_t SESSION_HEX_LEN = 32;    // 128-bit token

enum TxCheck : uint8_t { TX_OK = 0, TX_MISSING, TX_BAD_JSON, TX_TOO_LARGE };

// HTTP status for a TxCheck (0 = accept and queue).
inline int tx_check_status(TxCheck c) {
  switch (c) {
    case TX_MISSING:   return 400;
    case TX_BAD_JSON:  return 400;
    case TX_TOO_LARGE: return 413;
    default:           return 0;
  }
}

// Builds the LORA_MSG payload object exactly as it is sent (Contract 1).
inline void fill_lora_msg(JsonDocument& doc, const char* name, const char* mac, const char* data) {
  doc.clear();
  doc["name"] = name ? name : "";
  doc["mac"] = mac ? mac : "";
  doc["data"] = data ? data : "";
}

// Serialized length of the wrapped LORA_MSG payload.
inline size_t lora_msg_wrapped_len(const char* name, const char* mac, const char* data) {
  JsonDocument doc;
  fill_lora_msg(doc, name, mac, data);
  return measureJson(doc);
}

// /lora_transmit: data = inner JSON string (nullptr when the param is missing).
inline TxCheck check_lora_transmit(const char* data, const char* name, const char* mac,
                                   size_t max_payload, size_t* wrapped_out = nullptr) {
  if (data == nullptr || data[0] == '\0') return TX_MISSING;
  // Wrapping only adds bytes, so an oversized inner string can be rejected before
  // anything is allocated for it.
  if (strlen(data) > max_payload) return TX_TOO_LARGE;
  size_t wrapped = lora_msg_wrapped_len(name, mac, data);
  if (wrapped_out) *wrapped_out = wrapped;
  if (wrapped > max_payload) return TX_TOO_LARGE;
  JsonDocument inner;
  DeserializationError err = deserializeJson(inner, data);
  if (err || !inner.is<JsonObject>()) return TX_BAD_JSON;
  return TX_OK;
}

// /send_raw: data = one data-URL slice, sent as-is.
inline TxCheck check_send_raw(const char* data, size_t max_payload) {
  if (data == nullptr || data[0] == '\0') return TX_MISSING;
  if (strlen(data) > max_payload) return TX_TOO_LARGE;
  return TX_OK;
}

// /lora_transmit optional form field `ack`: only an explicit "0" turns the
// acknowledgement off (missing / anything else = wait for the ACK, as before).
inline bool ack_param_wants_ack(const char* v) {
  return !(v != nullptr && strcmp(v, "0") == 0);
}

// GET /api/config?name= whitelist (Contract 2).
inline bool is_allowed_config_name(const char* name) {
  if (name == nullptr) return false;
  static const char* const allowed[] = {
      "lora_config.json",  "long_lora_config.json", "short_lora_config.json",
      "config_list.json",  "lora_serial.json",      "wifi_config.json"};
  for (const char* a : allowed)
    if (strcmp(a, name) == 0) return true;
  return false;
}

inline bool is_wifi_config_name(const char* name) {
  return name != nullptr && strcmp(name, "wifi_config.json") == 0;
}

inline char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

inline bool ieq_n(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
  return true;
}

// True when a (URL-decoded) request path must never be served: anything under
// /config, however it is spelled ("//config/", "/./config", "/CONFIG/"), and any
// path with a ".." segment.
inline bool path_is_blocked(const char* url) {
  if (url == nullptr) return true;
  const char* p = url;
  bool first_real = true;
  while (*p) {
    while (*p == '/' || *p == '\\') p++;
    if (!*p) break;
    const char* s = p;
    while (*p && *p != '/' && *p != '\\' && *p != '?') p++;
    size_t n = (size_t)(p - s);
    if (n == 1 && s[0] == '.') continue;
    if (n == 2 && s[0] == '.' && s[1] == '.') return true;
    if (first_real) {
      if (n == 6 && ieq_n(s, "config", 6)) return true;
      first_real = false;
    }
    if (*p == '?') break;
  }
  return false;
}

// Removes secrets from a parsed wifi_config.json. False if it isn't an object.
inline bool redact_wifi_config(JsonDocument& doc) {
  if (!doc.is<JsonObject>()) return false;
  doc.remove("wifi_pass");
  doc.remove("ap_pass");
  return true;
}

// Origin check for state-changing requests and WS handshakes.
// - No Origin header (curl, the flasher, old clients): allowed. Browsers always
//   send Origin on cross-origin POST and on every WebSocket handshake.
// - "null" or a non-http(s) origin: rejected.
// - Otherwise host[:port] of the Origin must equal the Host header (case-insensitive).
inline bool origin_allowed(const char* origin, const char* host) {
  if (origin == nullptr || origin[0] == '\0') return true;
  if (host == nullptr || host[0] == '\0') return false;
  const char* a;
  if (ieq_n(origin, "http://", 7) && strlen(origin) > 7) a = origin + 7;
  else if (ieq_n(origin, "https://", 8) && strlen(origin) > 8) a = origin + 8;
  else return false;
  size_t alen = strlen(a);
  if (alen > 0 && a[alen - 1] == '/') alen--;
  if (alen != strlen(host)) return false;
  return ieq_n(a, host, alen);
}

// Accept only complete, single-frame text messages of at most max_len bytes.
// opcode 1 = WS_TEXT.
inline bool ws_frame_acceptable(bool final, uint64_t index, uint32_t num, uint64_t total_len,
                                size_t chunk_len, uint8_t opcode, size_t max_len = WS_MAX_MESSAGE) {
  return final && index == 0 && num == 0 && opcode == 1 && total_len == chunk_len &&
         chunk_len > 0 && chunk_len <= max_len;
}

inline bool password_valid(const char* p) {
  if (p == nullptr) return false;
  size_t n = strlen(p);
  return n >= PASSWORD_MIN_LEN && n <= PASSWORD_MAX_LEN;
}

inline bool is_session_token(const char* t, size_t n) {
  if (t == nullptr || n != SESSION_HEX_LEN) return false;
  for (size_t i = 0; i < n; i++) {
    char c = t[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

// Extracts cookie `name` from a Cookie header into out (NUL-terminated).
inline bool cookie_get(const char* header, const char* name, char* out, size_t out_size) {
  if (header == nullptr || name == nullptr || out == nullptr || out_size == 0) return false;
  size_t nlen = strlen(name);
  const char* p = header;
  while (*p) {
    while (*p == ' ' || *p == ';' || *p == '\t') p++;
    if (!*p) break;
    const char* k = p;
    while (*p && *p != '=' && *p != ';') p++;
    size_t klen = (size_t)(p - k);
    while (klen > 0 && k[klen - 1] == ' ') klen--;
    if (*p != '=') continue;
    p++;
    const char* v = p;
    while (*p && *p != ';') p++;
    size_t vlen = (size_t)(p - v);
    while (vlen > 0 && v[vlen - 1] == ' ') vlen--;
    if (klen == nlen && strncmp(k, name, nlen) == 0) {
      if (vlen + 1 > out_size) return false;
      memcpy(out, v, vlen);
      out[vlen] = '\0';
      return true;
    }
  }
  return false;
}

// Constant-time compare of two equal-length buffers.
inline bool ct_equal(const char* a, const char* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
  return d == 0;
}

// User settings carried across a LittleFS OTA (Contract 6). Each is snapshotted
// before the first write and restored after the remount.
static const char* const OTA_KEEP_FILES[] = {
    "/config/wifi_config.json", "/config/lora_config.json",
    "/config/user_data.json",   "/config/lora_serial.json",
    "/config/sensor_config.json"};
static const size_t OTA_KEEP_COUNT = sizeof(OTA_KEEP_FILES) / sizeof(OTA_KEEP_FILES[0]);
static const size_t OTA_KEEP_MAX_BYTES = 2048;

// A file is snapshotted only if it exists, is non-empty and is at most 2 KB.
// An oversized file is skipped whole: a truncated JSON file would be worse than
// the image's default.
inline bool ota_keep_size_ok(bool exists, size_t size) {
  return exists && size > 0 && size <= OTA_KEEP_MAX_BYTES;
}

// Next TX id after `cur`, never 0 (0 is reserved for device-originated packets).
inline uint32_t tx_id_after(uint32_t cur) {
  uint32_t n = cur + 1;
  return n == 0 ? 1 : n;
}

// Request types are logged, never the body. Keep the logged type short and printable.
inline void sanitize_for_log(const char* in, char* out, size_t out_size) {
  if (out_size == 0) return;
  size_t j = 0;
  if (in)
    for (size_t i = 0; in[i] && j + 1 < out_size; i++) {
      char c = in[i];
      out[j++] = (c >= 0x20 && c < 0x7f) ? c : '?';
    }
  out[j] = '\0';
}

}  // namespace wv
#endif
