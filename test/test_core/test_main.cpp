// L2 tests for system-core: WS dispatcher validation/routing, config parsing
// defaults and the button debounce/hold state machine (src/core_dispatch.h).
#include <unity.h>
#include <ArduinoJson.h>
#include <string>
#include "core/core_dispatch.h"

void setUp() {}
void tearDown() {}

static CoreCommand parse(const char* json) {
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, json);
    TEST_ASSERT_FALSE_MESSAGE(e, json);
    return core_parse_request(doc.as<JsonVariantConst>());
}

#define ASSERT_ACTION(expected, json) \
    TEST_ASSERT_EQUAL_STRING_MESSAGE(core_action_name(expected), core_action_name(parse(json).action), json)

static std::string str_of(size_t n, char c = 'a') { return std::string(n, c); }

static CoreCommand parse_doc(JsonDocument& doc) {
    return core_parse_request(doc.as<JsonVariantConst>());
}

// ------------------------------------------------------------ dispatcher

static void test_missing_request_type() {
    ASSERT_ACTION(CoreAction::Missing, "{}");
    ASSERT_ACTION(CoreAction::Missing, "{\"val\":\"x\"}");
    ASSERT_ACTION(CoreAction::Missing, "{\"request-type\":null}");
    ASSERT_ACTION(CoreAction::Missing, "{\"request-type\":5}");
    ASSERT_ACTION(CoreAction::Missing, "{\"request-type\":true}");
    ASSERT_ACTION(CoreAction::Missing, "{\"request-type\":[\"get_username\"]}");
    ASSERT_ACTION(CoreAction::Missing, "[]");
    ASSERT_ACTION(CoreAction::Missing, "\"get_username\"");
}

static void test_unknown_request_type() {
    ASSERT_ACTION(CoreAction::Unknown, "{\"request-type\":\"\"}");
    ASSERT_ACTION(CoreAction::Unknown, "{\"request-type\":\"bogus\"}");
    ASSERT_ACTION(CoreAction::Unknown, "{\"request-type\":\"send_akn\",\"akn\":1}");   // removed in v2
    ASSERT_ACTION(CoreAction::Unknown, "{\"request-type\":\"GET_USERNAME\"}");
    ASSERT_ACTION(CoreAction::Unknown, "{\"request-type\":\"set_sync_word\"}");
}

static void test_no_arg_commands() {
    ASSERT_ACTION(CoreAction::WifiScan, "{\"request-type\":\"wifi_ssid_scan\"}");
    ASSERT_ACTION(CoreAction::WifiApMode, "{\"request-type\":\"wifi_ap_mode\"}");
    ASSERT_ACTION(CoreAction::ResetDevice, "{\"request-type\":\"reset_device\"}");
    ASSERT_ACTION(CoreAction::RestartDevice, "{\"request-type\":\"restart_device\"}");
    ASSERT_ACTION(CoreAction::GetUsername, "{\"request-type\":\"get_username\"}");
    ASSERT_ACTION(CoreAction::GetSerialMode, "{\"request-type\":\"get_serial_mode\"}");
    ASSERT_ACTION(CoreAction::GetNetStatus, "{\"request-type\":\"get_net_status\"}");
}

static void test_get_wifi_scan() {
    CoreCommand c = parse("{\"request-type\":\"get_wifi_scan\"}");
    TEST_ASSERT_EQUAL(CoreAction::GetWifiScan, c.action);
    TEST_ASSERT_EQUAL_STRING("get_wifi_scan", core_action_name(c.action));
    // extra keys are ignored; it is a different command from wifi_ssid_scan
    ASSERT_ACTION(CoreAction::GetWifiScan, "{\"request-type\":\"get_wifi_scan\",\"val\":1}");
    ASSERT_ACTION(CoreAction::WifiScan, "{\"request-type\":\"wifi_ssid_scan\"}");
    ASSERT_ACTION(CoreAction::Unknown, "{\"request-type\":\"get_wifi_scan \"}");
    ASSERT_ACTION(CoreAction::Unknown, "{\"request-type\":\"GET_WIFI_SCAN\"}");
    ASSERT_ACTION(CoreAction::Missing, "{\"request-type\":[\"get_wifi_scan\"]}");
}

static void test_connect_wifi() {
    CoreCommand c = parse("{\"request-type\":\"connect_wifi\",\"wifi_ssid\":\"home\",\"wifi_pass\":\"secret123\"}");
    TEST_ASSERT_EQUAL(CoreAction::ConnectWifi, c.action);
    TEST_ASSERT_EQUAL_STRING("home", c.arg1.c_str());
    TEST_ASSERT_EQUAL_STRING("secret123", c.arg2.c_str());

    // open network: empty or missing pass
    c = parse("{\"request-type\":\"connect_wifi\",\"wifi_ssid\":\"cafe\",\"wifi_pass\":\"\"}");
    TEST_ASSERT_EQUAL(CoreAction::ConnectWifi, c.action);
    TEST_ASSERT_EQUAL_STRING("", c.arg2.c_str());
    c = parse("{\"request-type\":\"connect_wifi\",\"wifi_ssid\":\"cafe\"}");
    TEST_ASSERT_EQUAL(CoreAction::ConnectWifi, c.action);

    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"connect_wifi\"}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"connect_wifi\",\"wifi_ssid\":\"\"}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"connect_wifi\",\"wifi_ssid\":42}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"connect_wifi\",\"wifi_ssid\":\"x\",\"wifi_pass\":12345678}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"connect_wifi\",\"wifi_ssid\":\"x\",\"wifi_pass\":\"short\"}");

    JsonDocument d;
    d["request-type"] = "connect_wifi";
    d["wifi_ssid"] = str_of(32);
    d["wifi_pass"] = str_of(63);
    TEST_ASSERT_EQUAL(CoreAction::ConnectWifi, parse_doc(d).action);
    d["wifi_ssid"] = str_of(33);
    TEST_ASSERT_EQUAL(CoreAction::Invalid, parse_doc(d).action);
    d["wifi_ssid"] = "ok";
    d["wifi_pass"] = str_of(64);
    TEST_ASSERT_EQUAL(CoreAction::Invalid, parse_doc(d).action);
}

static void test_set_ap_pass() {
    JsonDocument d;
    d["request-type"] = "set_ap_pass";
    const size_t lens[] = {0, 7, 8, 63, 64};
    const CoreAction expect[] = {CoreAction::SetApPass, CoreAction::Invalid, CoreAction::SetApPass,
                                 CoreAction::SetApPass, CoreAction::Invalid};
    for (int i = 0; i < 5; i++) {
        d["val"] = str_of(lens[i], 'p');
        CoreCommand c = parse_doc(d);
        char msg[32];
        snprintf(msg, sizeof msg, "len=%u", (unsigned)lens[i]);
        TEST_ASSERT_EQUAL_MESSAGE(expect[i], c.action, msg);
        if (c.action == CoreAction::SetApPass)
            TEST_ASSERT_EQUAL_UINT32(lens[i], c.arg1.size());
    }
    // "" passes through as "" (derived default)
    CoreCommand c = parse("{\"request-type\":\"set_ap_pass\",\"val\":\"\"}");
    TEST_ASSERT_EQUAL(CoreAction::SetApPass, c.action);
    TEST_ASSERT_TRUE(c.arg1.empty());
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_ap_pass\"}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_ap_pass\",\"val\":null}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_ap_pass\",\"val\":123456789}");
}

static void test_set_username_lengths() {
    JsonDocument d;
    d["request-type"] = "set_username";
    const size_t lens[] = {0, 1, 32, 33};
    const CoreAction expect[] = {CoreAction::Invalid, CoreAction::SetUsername,
                                 CoreAction::SetUsername, CoreAction::Invalid};
    for (int i = 0; i < 4; i++) {
        d["val"] = str_of(lens[i], 'u');
        CoreCommand c = parse_doc(d);
        char msg[32];
        snprintf(msg, sizeof msg, "len=%u", (unsigned)lens[i]);
        TEST_ASSERT_EQUAL_MESSAGE(expect[i], c.action, msg);
    }
    CoreCommand c = parse("{\"request-type\":\"set_username\",\"val\":\"alice\"}");
    TEST_ASSERT_EQUAL_STRING("alice", c.arg1.c_str());
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_username\"}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_username\",\"val\":7}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_username\",\"val\":\"a\\nb\"}");
}

static const char* GOOD_LORA =
    "{\"freq\":510000000,\"TxPower\":10,\"SpreadingFactor\":8,\"SignalBandwidth\":125000,\"CodingRate4\":7,\"SyncWord\":52}";

static void lora_case(const char* key, long value, bool ok) {
    JsonDocument cfg;
    deserializeJson(cfg, GOOD_LORA);
    cfg[key] = value;
    std::string s;
    serializeJson(cfg, s);
    JsonDocument d;
    d["request-type"] = "set_lora_config";
    d["val"] = s;
    char msg[64];
    snprintf(msg, sizeof msg, "%s=%ld", key, value);
    TEST_ASSERT_EQUAL_MESSAGE(ok ? CoreAction::SetLoraConfig : CoreAction::Invalid, parse_doc(d).action, msg);
}

static void test_set_lora_config() {
    JsonDocument d;
    d["request-type"] = "set_lora_config";
    d["val"] = GOOD_LORA;
    CoreCommand c = parse_doc(d);
    TEST_ASSERT_EQUAL(CoreAction::SetLoraConfig, c.action);
    TEST_ASSERT_EQUAL_STRING(GOOD_LORA, c.arg1.c_str());

    lora_case("freq", 410000000, true);   lora_case("freq", 409999999, false);
    lora_case("freq", 525000000, true);   lora_case("freq", 525000001, false);
    lora_case("TxPower", 2, true);        lora_case("TxPower", 1, false);
    lora_case("TxPower", 20, true);       lora_case("TxPower", 21, false);
    lora_case("SpreadingFactor", 6, true); lora_case("SpreadingFactor", 5, false);
    lora_case("SpreadingFactor", 12, true); lora_case("SpreadingFactor", 13, false);
    lora_case("SignalBandwidth", 500000, true); lora_case("SignalBandwidth", 7800, true);
    lora_case("SignalBandwidth", 100000, false);
    lora_case("CodingRate4", 5, true);    lora_case("CodingRate4", 4, false);
    lora_case("CodingRate4", 8, true);    lora_case("CodingRate4", 9, false);
    lora_case("SyncWord", 0, true);       lora_case("SyncWord", 255, true);
    lora_case("SyncWord", -1, false);     lora_case("SyncWord", 256, false);

    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_lora_config\"}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_lora_config\",\"val\":{\"freq\":1}}");   // object, not string
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_lora_config\",\"val\":\"not json\"}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_lora_config\",\"val\":\"[]\"}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_lora_config\",\"val\":\"{}\"}");
    // wrong value types inside the config
    ASSERT_ACTION(CoreAction::Invalid,
        "{\"request-type\":\"set_lora_config\",\"val\":\"{\\\"freq\\\":\\\"510000000\\\",\\\"TxPower\\\":10,\\\"SpreadingFactor\\\":8,\\\"SignalBandwidth\\\":125000,\\\"CodingRate4\\\":7,\\\"SyncWord\\\":52}\"}");
    ASSERT_ACTION(CoreAction::Invalid,
        "{\"request-type\":\"set_lora_config\",\"val\":\"{\\\"freq\\\":510000000,\\\"TxPower\\\":10.5,\\\"SpreadingFactor\\\":8,\\\"SignalBandwidth\\\":125000,\\\"CodingRate4\\\":7,\\\"SyncWord\\\":52}\"}");
    ASSERT_ACTION(CoreAction::Invalid,
        "{\"request-type\":\"set_lora_config\",\"val\":\"{\\\"freq\\\":510000000,\\\"TxPower\\\":true,\\\"SpreadingFactor\\\":8,\\\"SignalBandwidth\\\":125000,\\\"CodingRate4\\\":7,\\\"SyncWord\\\":52}\"}");
}

static void test_set_serial_mode() {
    CoreCommand c = parse("{\"request-type\":\"set_serial_mode\",\"value\":true}");
    TEST_ASSERT_EQUAL(CoreAction::SetSerialMode, c.action);
    TEST_ASSERT_TRUE(c.flag);
    c = parse("{\"request-type\":\"set_serial_mode\",\"value\":false}");
    TEST_ASSERT_EQUAL(CoreAction::SetSerialMode, c.action);
    TEST_ASSERT_FALSE(c.flag);
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_serial_mode\"}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_serial_mode\",\"value\":1}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_serial_mode\",\"value\":\"true\"}");
}

// ---------------------------------------------------- Bluetooth Serial commands

static void test_bt_no_arg_commands() {
    ASSERT_ACTION(CoreAction::GetBtConfig, "{\"request-type\":\"get_bt_config\"}");
    ASSERT_ACTION(CoreAction::RebootToBt, "{\"request-type\":\"reboot_to_bt\"}");
    ASSERT_ACTION(CoreAction::FileTxStart, "{\"request-type\":\"file_tx_start\"}");
    ASSERT_ACTION(CoreAction::FileTxEnd, "{\"request-type\":\"file_tx_end\"}");
}

static void test_set_bt_mode() {
    CoreCommand c = parse("{\"request-type\":\"set_bt_mode\",\"value\":true}");
    TEST_ASSERT_EQUAL(CoreAction::SetBtMode, c.action);
    TEST_ASSERT_TRUE(c.flag);
    c = parse("{\"request-type\":\"set_bt_mode\",\"value\":false}");
    TEST_ASSERT_EQUAL(CoreAction::SetBtMode, c.action);
    TEST_ASSERT_FALSE(c.flag);
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_bt_mode\"}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_bt_mode\",\"value\":1}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_bt_mode\",\"value\":\"true\"}");
}

static void test_file_tx_invite() {
    CoreCommand c = parse("{\"request-type\":\"file_tx_invite\",\"fileName\":\"test.bin\",\"fileSize\":1024}");
    TEST_ASSERT_EQUAL(CoreAction::FileTxInvite, c.action);
    TEST_ASSERT_EQUAL_STRING("test.bin", c.arg1.c_str());
    TEST_ASSERT_EQUAL_STRING("1024", c.arg2.c_str());
    // fileSize as string
    c = parse("{\"request-type\":\"file_tx_invite\",\"fileName\":\"data.txt\",\"fileSize\":\"2048\"}");
    TEST_ASSERT_EQUAL(CoreAction::FileTxInvite, c.action);
    TEST_ASSERT_EQUAL_STRING("2048", c.arg2.c_str());
    // missing args
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"file_tx_invite\"}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"file_tx_invite\",\"fileName\":\"test.bin\"}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"file_tx_invite\",\"fileSize\":1024}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"file_tx_invite\",\"fileName\":123,\"fileSize\":1024}");
}

static void test_file_tx_join() {
    CoreCommand c = parse("{\"request-type\":\"file_tx_join\",\"sender_mac\":\"AA:BB:CC:DD:EE:FF\",\"mode\":\"usb\"}");
    TEST_ASSERT_EQUAL(CoreAction::FileTxJoin, c.action);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", c.arg1.c_str());
    TEST_ASSERT_EQUAL_STRING("usb", c.arg2.c_str());
    c = parse("{\"request-type\":\"file_tx_join\",\"sender_mac\":\"11:22:33:44:55:66\",\"mode\":\"bt\"}");
    TEST_ASSERT_EQUAL(CoreAction::FileTxJoin, c.action);
    TEST_ASSERT_EQUAL_STRING("bt", c.arg2.c_str());
    // invalid mode
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"file_tx_join\",\"sender_mac\":\"AA:BB:CC:DD:EE:FF\",\"mode\":\"serial\"}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"file_tx_join\",\"sender_mac\":\"AA:BB:CC:DD:EE:FF\",\"mode\":\"USB\"}");
    // missing args
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"file_tx_join\"}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"file_tx_join\",\"sender_mac\":\"AA:BB:CC:DD:EE:FF\"}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"file_tx_join\",\"mode\":\"usb\"}");
}

static void test_sensor_config_commands() {
    ASSERT_ACTION(CoreAction::GetSensorConfig, "{\"request-type\":\"get_sensor_config\"}");
    CoreCommand c = parse("{\"request-type\":\"set_sensor_config\",\"val\":\"{\\\"enabled\\\":false}\"}");
    TEST_ASSERT_EQUAL(CoreAction::SetSensorConfig, c.action);
    TEST_ASSERT_EQUAL_STRING("{\"enabled\":false}", c.arg1.c_str());
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_sensor_config\"}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_sensor_config\",\"val\":\"\"}");
    ASSERT_ACTION(CoreAction::Invalid, "{\"request-type\":\"set_sensor_config\",\"val\":{\"enabled\":true}}");
    JsonDocument doc;
    doc["request-type"] = "set_sensor_config";
    doc["val"] = std::string(CORE_SENSOR_CONFIG_MAX + 1, 'x');
    TEST_ASSERT_EQUAL(CoreAction::Invalid, parse_doc(doc).action);
}

static void test_invalid_error_has_no_secret() {
    CoreCommand c = parse("{\"request-type\":\"connect_wifi\",\"wifi_ssid\":\"x\",\"wifi_pass\":\"sekret\"}");
    TEST_ASSERT_EQUAL(CoreAction::Invalid, c.action);
    TEST_ASSERT_NULL(strstr(c.error, "sekret"));
}

// --------------------------------------------------------- config parsing

static void test_parse_username() {
    const std::string mac = "AA:BB:CC:DD:EE:FF";
    TEST_ASSERT_EQUAL_STRING("bob", core_parse_username("{\"username\":\"bob\"}", mac).c_str());
    TEST_ASSERT_EQUAL_STRING(mac.c_str(), core_parse_username(nullptr, mac).c_str());
    TEST_ASSERT_EQUAL_STRING(mac.c_str(), core_parse_username("", mac).c_str());          // empty file
    TEST_ASSERT_EQUAL_STRING(mac.c_str(), core_parse_username("{", mac).c_str());         // parse error
    TEST_ASSERT_EQUAL_STRING(mac.c_str(), core_parse_username("{}", mac).c_str());        // missing key
    TEST_ASSERT_EQUAL_STRING(mac.c_str(), core_parse_username("{\"username\":\"\"}", mac).c_str());
    TEST_ASSERT_EQUAL_STRING(mac.c_str(), core_parse_username("{\"username\":null}", mac).c_str());
    TEST_ASSERT_EQUAL_STRING(mac.c_str(), core_parse_username("{\"username\":5}", mac).c_str());
    std::string long_name = "{\"username\":\"" + str_of(33) + "\"}";
    TEST_ASSERT_EQUAL_STRING(mac.c_str(), core_parse_username(long_name.c_str(), mac).c_str());
    std::string max_name = "{\"username\":\"" + str_of(32) + "\"}";
    TEST_ASSERT_EQUAL_STRING(str_of(32).c_str(), core_parse_username(max_name.c_str(), mac).c_str());
}

static void test_parse_lora_serial() {
    TEST_ASSERT_TRUE(core_parse_lora_serial("{\"lora_serial\":true}"));
    TEST_ASSERT_FALSE(core_parse_lora_serial("{\"lora_serial\":false}"));
    TEST_ASSERT_FALSE(core_parse_lora_serial(nullptr));
    TEST_ASSERT_FALSE(core_parse_lora_serial(""));
    TEST_ASSERT_FALSE(core_parse_lora_serial("{"));
    TEST_ASSERT_FALSE(core_parse_lora_serial("{}"));
    TEST_ASSERT_FALSE(core_parse_lora_serial("{\"lora_serial\":1}"));
    TEST_ASSERT_FALSE(core_parse_lora_serial("{\"lora_serial\":\"true\"}"));
}

static void test_parse_device_mode() {
    TEST_ASSERT_EQUAL_STRING("WiFi", core_parse_device_mode(nullptr).c_str());
    TEST_ASSERT_EQUAL_STRING("WiFi", core_parse_device_mode("garbage").c_str());
    TEST_ASSERT_EQUAL_STRING("WiFi", core_parse_device_mode("{}").c_str());
    TEST_ASSERT_EQUAL_STRING("WiFi", core_parse_device_mode("{\"device_mode\":\"\"}").c_str());
    TEST_ASSERT_EQUAL_STRING("LoRa", core_parse_device_mode("{\"device_mode\":\"LoRa\"}").c_str());
}

// ---------------------------------------------------------- button machine

// Drive a button with `pressed` from t0 to t1 (exclusive) in 10 ms steps,
// recording which events happened.
struct Seen { int pressed = 0, progress = 0, fired = 0, short_rel = 0, long_rel = 0; uint8_t max_pct = 0; };

static uint32_t drive(CoreButton& b, bool pressed, uint32_t t0, uint32_t t1, Seen& s) {
    for (uint32_t t = t0; t < t1; t += 10) {
        BtnOutput o = b.update(pressed, t);
        switch (o.ev) {
            case BtnEvent::Pressed: s.pressed++; break;
            case BtnEvent::Progress: s.progress++; if (o.pct > s.max_pct) s.max_pct = o.pct; break;
            case BtnEvent::HoldFired: s.fired++; break;
            case BtnEvent::ShortRelease: s.short_rel++; break;
            case BtnEvent::LongRelease: s.long_rel++; break;
            default: break;
        }
    }
    return t1;
}

static void test_btn_bounce_ignored() {
    CoreButton b(CORE_SOS_HOLD_MS);
    Seen s;
    // 10 ms glitches never last the 30 ms debounce window
    for (uint32_t t = 0; t < 500; t += 10) {
        BtnOutput o = b.update((t / 10) % 2 == 0, t);
        TEST_ASSERT_EQUAL(BtnEvent::None, o.ev);
    }
    (void)s;
}

static void test_btn1_hold_fires_once() {
    CoreButton b(CORE_SOS_HOLD_MS);
    Seen s;
    uint32_t t = drive(b, false, 0, 100, s);
    t = drive(b, true, t, t + 5000, s);
    TEST_ASSERT_EQUAL(1, s.pressed);
    TEST_ASSERT_EQUAL(1, s.fired);
    TEST_ASSERT_TRUE(s.progress > 10);
    TEST_ASSERT_TRUE(s.max_pct >= 95 && s.max_pct < 100);
    drive(b, false, t, t + 200, s);
    TEST_ASSERT_EQUAL(1, s.long_rel);
    TEST_ASSERT_EQUAL(0, s.short_rel);
}

static void test_btn1_early_release_cancels() {
    CoreButton b(CORE_SOS_HOLD_MS);
    Seen s;
    uint32_t t = drive(b, true, 0, 1900, s);
    drive(b, false, t, t + 3000, s);
    TEST_ASSERT_EQUAL(0, s.fired);
    TEST_ASSERT_EQUAL(1, s.short_rel);
}

static void test_btn1_fires_at_2s() {
    CoreButton b(CORE_SOS_HOLD_MS);
    uint32_t fired_at = 0;
    for (uint32_t t = 0; t < 3000; t += 10)
        if (b.update(true, t).ev == BtnEvent::HoldFired) { fired_at = t; break; }
    // pressed edge at 30 ms (debounce) + 2000 ms hold
    TEST_ASSERT_UINT32_WITHIN(20, 2030, fired_at);
}

static void test_btn2_short_and_long() {
    CoreButton b(CORE_BTN2_LONG_MS);
    Seen s;
    uint32_t t = drive(b, true, 0, 300, s);
    t = drive(b, false, t, t + 200, s);
    TEST_ASSERT_EQUAL(1, s.short_rel);
    TEST_ASSERT_EQUAL(0, s.fired);
    t = drive(b, true, t, t + 1500, s);
    TEST_ASSERT_EQUAL(1, s.fired);
    drive(b, false, t, t + 200, s);
    TEST_ASSERT_EQUAL(1, s.long_rel);
    TEST_ASSERT_EQUAL(1, s.short_rel);
    TEST_ASSERT_EQUAL(2, s.pressed);
}

static void test_btn_millis_wrap() {
    CoreButton b(CORE_SOS_HOLD_MS);
    Seen s;
    uint32_t t0 = 0xFFFFFFFFu - 1000;
    // run across the 32-bit wrap
    uint32_t t = t0;
    for (int i = 0; i < 300; i++, t += 10) {
        BtnOutput o = b.update(true, t);
        if (o.ev == BtnEvent::HoldFired) s.fired++;
    }
    TEST_ASSERT_EQUAL(1, s.fired);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_missing_request_type);
    RUN_TEST(test_unknown_request_type);
    RUN_TEST(test_no_arg_commands);
    RUN_TEST(test_get_wifi_scan);
    RUN_TEST(test_connect_wifi);
    RUN_TEST(test_set_ap_pass);
    RUN_TEST(test_set_username_lengths);
    RUN_TEST(test_set_lora_config);
    RUN_TEST(test_set_serial_mode);
    RUN_TEST(test_sensor_config_commands);
    RUN_TEST(test_invalid_error_has_no_secret);
    RUN_TEST(test_bt_no_arg_commands);
    RUN_TEST(test_set_bt_mode);
    RUN_TEST(test_file_tx_invite);
    RUN_TEST(test_file_tx_join);
    RUN_TEST(test_parse_username);
    RUN_TEST(test_parse_lora_serial);
    RUN_TEST(test_parse_device_mode);
    RUN_TEST(test_btn_bounce_ignored);
    RUN_TEST(test_btn1_hold_fires_once);
    RUN_TEST(test_btn1_early_release_cancels);
    RUN_TEST(test_btn1_fires_at_2s);
    RUN_TEST(test_btn2_short_and_long);
    RUN_TEST(test_btn_millis_wrap);
    return UNITY_END();
}
