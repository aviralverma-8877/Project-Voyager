// L2 tests for the web API validators (src/web_validate.h). Owner: web-api-engineer.
#include <unity.h>
#include <ArduinoJson.h>
#include <string>
#include "web/web_validate.h"

static const size_t MAX = 251;   // LORA_MAX_PAYLOAD (Contract 1)
static const char* NAME = "alice";
static const char* MAC = "AA:BB:CC:DD:EE:FF";

void setUp() {}
void tearDown() {}

/* ---- param presence + wrapped size ---- */

static void test_lora_transmit_missing() {
  TEST_ASSERT_EQUAL(wv::TX_MISSING, wv::check_lora_transmit(nullptr, NAME, MAC, MAX));
  TEST_ASSERT_EQUAL(wv::TX_MISSING, wv::check_lora_transmit("", NAME, MAC, MAX));
  TEST_ASSERT_EQUAL_INT(400, wv::tx_check_status(wv::TX_MISSING));
}

static void test_lora_transmit_ok_and_wrapped_len_matches_serialization() {
  const char* inner = "{\"pack_type\":\"msg\",\"data\":\"hi \\\"there\\\"\"}";
  size_t wrapped = 0;
  TEST_ASSERT_EQUAL(wv::TX_OK, wv::check_lora_transmit(inner, NAME, MAC, MAX, &wrapped));
  JsonDocument doc;
  wv::fill_lora_msg(doc, NAME, MAC, inner);
  std::string out;
  serializeJson(doc, out);
  TEST_ASSERT_EQUAL_size_t(out.size(), wrapped);
  TEST_ASSERT_EQUAL_size_t(out.size(), wv::lora_msg_wrapped_len(NAME, MAC, inner));
  TEST_ASSERT_EQUAL_INT(0, wv::tx_check_status(wv::TX_OK));
}

// Builds an inner JSON whose wrapped length is exactly `target`.
static std::string inner_with_wrapped_len(size_t target) {
  std::string base = "{\"pack_type\":\"msg\",\"data\":\"\"}";
  size_t w0 = wv::lora_msg_wrapped_len(NAME, MAC, base.c_str());
  // each plain char added inside data adds 1 to inner and 1 to wrapped
  std::string text(target - w0, 'x');
  std::string inner = "{\"pack_type\":\"msg\",\"data\":\"" + text + "\"}";
  TEST_ASSERT_EQUAL_size_t(target, wv::lora_msg_wrapped_len(NAME, MAC, inner.c_str()));
  return inner;
}

static void test_lora_transmit_boundary_251_252() {
  std::string at = inner_with_wrapped_len(251);
  TEST_ASSERT_EQUAL(wv::TX_OK, wv::check_lora_transmit(at.c_str(), NAME, MAC, MAX));
  std::string over = inner_with_wrapped_len(252);
  TEST_ASSERT_EQUAL(wv::TX_TOO_LARGE, wv::check_lora_transmit(over.c_str(), NAME, MAC, MAX));
  TEST_ASSERT_EQUAL_INT(413, wv::tx_check_status(wv::TX_TOO_LARGE));
}

static void test_lora_transmit_escaping_counts() {
  // Quotes inside the inner string are escaped when wrapped: inner fits, wrapped doesn't.
  std::string inner = "{\"pack_type\":\"msg\",\"data\":\"" + std::string(90, 'x') + "\"}";
  // ~190-char inner made of escaped quotes: <= 251 raw, > 251 once escaped again
  std::string q = "{\"d\":\"";
  for (int i = 0; i < 90; i++) q += "\\\"";
  q += "\"}";
  TEST_ASSERT_TRUE(q.size() <= MAX);
  TEST_ASSERT_EQUAL(wv::TX_TOO_LARGE, wv::check_lora_transmit(q.c_str(), NAME, MAC, MAX));
  TEST_ASSERT_EQUAL(wv::TX_OK, wv::check_lora_transmit(inner.c_str(), NAME, MAC, MAX));
}

static void test_lora_transmit_300_bytes_is_413() {
  std::string inner = "{\"pack_type\":\"msg\",\"data\":\"" + std::string(300, 'x') + "\"}";
  TEST_ASSERT_EQUAL(wv::TX_TOO_LARGE, wv::check_lora_transmit(inner.c_str(), NAME, MAC, MAX));
}

static void test_lora_transmit_bad_json() {
  TEST_ASSERT_EQUAL(wv::TX_BAD_JSON, wv::check_lora_transmit("{not json", NAME, MAC, MAX));
  TEST_ASSERT_EQUAL(wv::TX_BAD_JSON, wv::check_lora_transmit("\"just a string\"", NAME, MAC, MAX));
  TEST_ASSERT_EQUAL_INT(400, wv::tx_check_status(wv::TX_BAD_JSON));
}

static void test_send_raw() {
  TEST_ASSERT_EQUAL(wv::TX_MISSING, wv::check_send_raw(nullptr, MAX));
  TEST_ASSERT_EQUAL(wv::TX_MISSING, wv::check_send_raw("", MAX));
  TEST_ASSERT_EQUAL(wv::TX_OK, wv::check_send_raw(std::string(200, 'a').c_str(), MAX));
  TEST_ASSERT_EQUAL(wv::TX_OK, wv::check_send_raw(std::string(251, 'a').c_str(), MAX));
  TEST_ASSERT_EQUAL(wv::TX_TOO_LARGE, wv::check_send_raw(std::string(252, 'a').c_str(), MAX));
}

/* ---- config whitelist, path blocking, redaction ---- */

static void test_config_whitelist() {
  const char* ok[] = {"lora_config.json", "long_lora_config.json", "short_lora_config.json",
                      "config_list.json", "lora_serial.json",      "wifi_config.json"};
  for (const char* n : ok) TEST_ASSERT_TRUE_MESSAGE(wv::is_allowed_config_name(n), n);
  const char* bad[] = {nullptr, "", "wifi_default.json", "user_data.json", "../index.html",
                       "/config/wifi_config.json", "wifi_config.json.tmp", "WIFI_CONFIG.JSON",
                       "lora_config.json/"};
  for (const char* n : bad) TEST_ASSERT_FALSE(wv::is_allowed_config_name(n));
  TEST_ASSERT_TRUE(wv::is_wifi_config_name("wifi_config.json"));
  TEST_ASSERT_FALSE(wv::is_wifi_config_name("lora_config.json"));
}

static void test_path_blocking() {
  const char* blocked[] = {"/config",       "/config/",          "/config/wifi_config.json",
                           "//config/x",    "/./config/x",       "/CONFIG/wifi_config.json",
                           "/Config",       "\\config\\x",       "/a/../config/x",
                           "/..",           "/config?x=1",       nullptr};
  for (const char* p : blocked) TEST_ASSERT_TRUE_MESSAGE(wv::path_is_blocked(p), p ? p : "null");
  const char* allowed[] = {"/",           "/index.html", "/script.js", "/configuration.html",
                           "/config.json", "/lib.js",    "/x/config/y", "/wifi.html"};
  for (const char* p : allowed) TEST_ASSERT_FALSE_MESSAGE(wv::path_is_blocked(p), p);
}

static void test_wifi_redaction() {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc,
      "{\"wifi_function\":\"STA\",\"wifi_ssid\":\"home\",\"wifi_pass\":\"secret123\",\"ap_pass\":\"apsecret\"}"));
  TEST_ASSERT_TRUE(wv::redact_wifi_config(doc));
  std::string out;
  serializeJson(doc, out);
  TEST_ASSERT_EQUAL_STRING("{\"wifi_function\":\"STA\",\"wifi_ssid\":\"home\"}", out.c_str());
  TEST_ASSERT_TRUE(out.find("secret") == std::string::npos);

  JsonDocument arr;
  deserializeJson(arr, "[1,2]");
  TEST_ASSERT_FALSE(wv::redact_wifi_config(arr));
}

/* ---- Origin ---- */

static void test_origin() {
  TEST_ASSERT_TRUE(wv::origin_allowed(nullptr, "192.168.4.1"));
  TEST_ASSERT_TRUE(wv::origin_allowed("", "192.168.4.1"));
  TEST_ASSERT_TRUE(wv::origin_allowed("http://192.168.4.1", "192.168.4.1"));
  TEST_ASSERT_TRUE(wv::origin_allowed("http://Voyager.local", "voyager.local"));
  TEST_ASSERT_TRUE(wv::origin_allowed("http://192.168.4.1:8080", "192.168.4.1:8080"));
  TEST_ASSERT_TRUE(wv::origin_allowed("https://dev.local/", "dev.local"));
  TEST_ASSERT_FALSE(wv::origin_allowed("http://evil.com", "192.168.4.1"));
  TEST_ASSERT_FALSE(wv::origin_allowed("http://192.168.4.1.evil.com", "192.168.4.1"));
  TEST_ASSERT_FALSE(wv::origin_allowed("http://192.168.4.1:81", "192.168.4.1"));
  TEST_ASSERT_FALSE(wv::origin_allowed("null", "192.168.4.1"));
  TEST_ASSERT_FALSE(wv::origin_allowed("file://", "192.168.4.1"));
  TEST_ASSERT_FALSE(wv::origin_allowed("http://", "192.168.4.1"));
  TEST_ASSERT_FALSE(wv::origin_allowed("http://192.168.4.1", ""));
  TEST_ASSERT_FALSE(wv::origin_allowed("http://192.168.4.1", nullptr));
}

/* ---- WS frame acceptance ---- */

static void test_ws_frames() {
  // final, index, num, total_len, chunk_len, opcode
  TEST_ASSERT_TRUE(wv::ws_frame_acceptable(true, 0, 0, 100, 100, 1));
  TEST_ASSERT_TRUE(wv::ws_frame_acceptable(true, 0, 0, 2048, 2048, 1));
  TEST_ASSERT_FALSE(wv::ws_frame_acceptable(true, 0, 0, 2049, 2049, 1));     // > 2 KB
  TEST_ASSERT_FALSE(wv::ws_frame_acceptable(false, 0, 0, 100, 100, 1));      // fragmented
  TEST_ASSERT_FALSE(wv::ws_frame_acceptable(true, 0, 1, 100, 100, 0));       // continuation frame
  TEST_ASSERT_FALSE(wv::ws_frame_acceptable(true, 50, 0, 100, 50, 1));       // later chunk
  TEST_ASSERT_FALSE(wv::ws_frame_acceptable(true, 0, 0, 100, 50, 1));        // first partial chunk
  TEST_ASSERT_FALSE(wv::ws_frame_acceptable(true, 0, 0, 100, 100, 2));       // binary
  TEST_ASSERT_FALSE(wv::ws_frame_acceptable(true, 0, 0, 0, 0, 1));           // empty
}

/* ---- auth helpers ---- */

static void test_password_length() {
  TEST_ASSERT_FALSE(wv::password_valid(nullptr));
  TEST_ASSERT_FALSE(wv::password_valid("1234567"));
  TEST_ASSERT_TRUE(wv::password_valid("12345678"));
  TEST_ASSERT_TRUE(wv::password_valid(std::string(63, 'p').c_str()));
  TEST_ASSERT_FALSE(wv::password_valid(std::string(64, 'p').c_str()));
}

static void test_cookie_parsing() {
  char out[40];
  const char* tok = "0123456789abcdef0123456789abcdef";
  std::string h = std::string("theme=dark; vsid=") + tok + "; other=1";
  TEST_ASSERT_TRUE(wv::cookie_get(h.c_str(), "vsid", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING(tok, out);
  TEST_ASSERT_TRUE(wv::is_session_token(out, strlen(out)));
  TEST_ASSERT_FALSE(wv::cookie_get("xvsid=abc; vsidx=def", "vsid", out, sizeof(out)));
  TEST_ASSERT_FALSE(wv::cookie_get(nullptr, "vsid", out, sizeof(out)));
  TEST_ASSERT_FALSE(wv::cookie_get("vsid", "vsid", out, sizeof(out)));
  std::string big = "vsid=" + std::string(100, 'a');
  TEST_ASSERT_FALSE(wv::cookie_get(big.c_str(), "vsid", out, sizeof(out)));   // no overflow
  TEST_ASSERT_TRUE(wv::cookie_get("vsid=", "vsid", out, sizeof(out)));
  TEST_ASSERT_FALSE(wv::is_session_token(out, strlen(out)));
  TEST_ASSERT_FALSE(wv::is_session_token("0123456789ABCDEF0123456789abcdef", 32));
}

static void test_ct_equal_and_ids() {
  TEST_ASSERT_TRUE(wv::ct_equal("abcd", "abcd", 4));
  TEST_ASSERT_FALSE(wv::ct_equal("abcd", "abce", 4));
  TEST_ASSERT_EQUAL_UINT32(1, wv::tx_id_after(0));
  TEST_ASSERT_EQUAL_UINT32(2, wv::tx_id_after(1));
  TEST_ASSERT_EQUAL_UINT32(1, wv::tx_id_after(0xFFFFFFFFu));   // never 0
}

static void test_log_sanitize() {
  char out[8];
  wv::sanitize_for_log("connect_wifi", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("connect", out);
  wv::sanitize_for_log("a\nb", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("a?b", out);
  wv::sanitize_for_log(nullptr, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("", out);
}

/* ---- LittleFS OTA settings snapshot (Contract 6) ---- */

static void test_ota_keep_files() {
  TEST_ASSERT_EQUAL_size_t(4, wv::OTA_KEEP_COUNT);
  const char* expect[] = {"/config/wifi_config.json", "/config/lora_config.json",
                          "/config/user_data.json", "/config/lora_serial.json"};
  for (size_t i = 0; i < 4; i++) TEST_ASSERT_EQUAL_STRING(expect[i], wv::OTA_KEEP_FILES[i]);
  TEST_ASSERT_EQUAL_size_t(2048, wv::OTA_KEEP_MAX_BYTES);
}

static void test_ota_keep_size_cap() {
  TEST_ASSERT_FALSE(wv::ota_keep_size_ok(false, 100));   // missing -> skipped
  TEST_ASSERT_FALSE(wv::ota_keep_size_ok(true, 0));      // empty -> skipped
  TEST_ASSERT_TRUE(wv::ota_keep_size_ok(true, 1));
  TEST_ASSERT_TRUE(wv::ota_keep_size_ok(true, 2048));
  TEST_ASSERT_FALSE(wv::ota_keep_size_ok(true, 2049));   // over cap -> skipped whole
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_lora_transmit_missing);
  RUN_TEST(test_lora_transmit_ok_and_wrapped_len_matches_serialization);
  RUN_TEST(test_lora_transmit_boundary_251_252);
  RUN_TEST(test_lora_transmit_escaping_counts);
  RUN_TEST(test_lora_transmit_300_bytes_is_413);
  RUN_TEST(test_lora_transmit_bad_json);
  RUN_TEST(test_send_raw);
  RUN_TEST(test_config_whitelist);
  RUN_TEST(test_path_blocking);
  RUN_TEST(test_wifi_redaction);
  RUN_TEST(test_origin);
  RUN_TEST(test_ws_frames);
  RUN_TEST(test_password_length);
  RUN_TEST(test_cookie_parsing);
  RUN_TEST(test_ct_equal_and_ids);
  RUN_TEST(test_log_sanitize);
  RUN_TEST(test_ota_keep_files);
  RUN_TEST(test_ota_keep_size_cap);
  return UNITY_END();
}
