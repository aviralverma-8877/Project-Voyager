// L2 tests for src/wifi_config.h (wifi-network-engineer).
#include <unity.h>
#include <string>
#include "wifi/wifi_config.h"

void setUp() {}
void tearDown() {}

static WifiParseResult parse(const char* j, WifiConfig& c) { return wifi_parse_config(std::string(j), c); }

static void test_parse_ap_ok() {
    WifiConfig c;
    TEST_ASSERT_EQUAL(WIFI_PARSE_OK, parse("{\"wifi_function\":\"AP\",\"wifi_ssid\":\"MyAP\",\"wifi_pass\":\"\",\"ap_pass\":\"\"}", c));
    TEST_ASSERT_EQUAL(WIFI_CFG_AP, c.mode);
    TEST_ASSERT_EQUAL_STRING("MyAP", c.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("", c.ap_pass.c_str());
}

static void test_parse_sta_ok() {
    WifiConfig c;
    TEST_ASSERT_EQUAL(WIFI_PARSE_OK, parse("{\"wifi_function\":\"STA\",\"wifi_ssid\":\"Home\",\"wifi_pass\":\"secret123\",\"ap_pass\":\"apsecret1\"}", c));
    TEST_ASSERT_EQUAL(WIFI_CFG_STA, c.mode);
    TEST_ASSERT_EQUAL_STRING("Home", c.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("secret123", c.pass.c_str());
    TEST_ASSERT_EQUAL_STRING("apsecret1", c.ap_pass.c_str());
}

static void test_parse_sta_open_network() {
    WifiConfig c;
    TEST_ASSERT_EQUAL(WIFI_PARSE_OK, parse("{\"wifi_function\":\"STA\",\"wifi_ssid\":\"Cafe\",\"wifi_pass\":\"\",\"ap_pass\":\"\"}", c));
    TEST_ASSERT_EQUAL(WIFI_CFG_STA, c.mode);
    TEST_ASSERT_EQUAL_STRING("", c.pass.c_str());
}

static void test_parse_errors() {
    WifiConfig c;
    TEST_ASSERT_EQUAL(WIFI_PARSE_ERROR, parse("", c));
    TEST_ASSERT_EQUAL(WIFI_PARSE_ERROR, parse("{not json", c));
    TEST_ASSERT_EQUAL(WIFI_PARSE_ERROR, parse("[1,2]", c));
    TEST_ASSERT_EQUAL(WIFI_PARSE_ERROR, parse("\"str\"", c));
    TEST_ASSERT_EQUAL(WIFI_PARSE_ERROR, wifi_parse_config(nullptr, 0, c));
    // defaults after an error
    TEST_ASSERT_EQUAL(WIFI_CFG_AP, c.mode);
    TEST_ASSERT_EQUAL_STRING(WIFI_DEFAULT_AP_SSID, c.ssid.c_str());
}

static void test_parse_missing_keys_default() {
    WifiConfig c;
    TEST_ASSERT_EQUAL(WIFI_PARSE_DEFAULTED, parse("{}", c));
    TEST_ASSERT_EQUAL(WIFI_CFG_AP, c.mode);
    TEST_ASSERT_EQUAL_STRING("Voyager", c.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("", c.ap_pass.c_str());
}

static void test_parse_unknown_mode_is_ap() {
    WifiConfig c;
    TEST_ASSERT_EQUAL(WIFI_PARSE_DEFAULTED, parse("{\"wifi_function\":\"MESH\",\"wifi_ssid\":\"X\",\"ap_pass\":\"\"}", c));
    TEST_ASSERT_EQUAL(WIFI_CFG_AP, c.mode);
    TEST_ASSERT_EQUAL_STRING("X", c.ssid.c_str());
}

static void test_parse_sta_bad_ssid_falls_back_to_ap() {
    WifiConfig c;
    TEST_ASSERT_EQUAL(WIFI_PARSE_DEFAULTED, parse("{\"wifi_function\":\"STA\",\"wifi_ssid\":\"\",\"wifi_pass\":\"secret123\",\"ap_pass\":\"\"}", c));
    TEST_ASSERT_EQUAL(WIFI_CFG_AP, c.mode);
    TEST_ASSERT_EQUAL_STRING("Voyager", c.ssid.c_str());
    TEST_ASSERT_EQUAL(WIFI_PARSE_DEFAULTED, parse("{\"wifi_function\":\"STA\",\"wifi_ssid\":\"0123456789012345678901234567890123\",\"wifi_pass\":\"\",\"ap_pass\":\"\"}", c));
    TEST_ASSERT_EQUAL(WIFI_CFG_AP, c.mode);
    TEST_ASSERT_EQUAL(WIFI_PARSE_DEFAULTED, parse("{\"wifi_function\":\"STA\",\"wifi_ssid\":5,\"ap_pass\":\"\"}", c));
    TEST_ASSERT_EQUAL(WIFI_CFG_AP, c.mode);
}

static void test_parse_sta_bad_pass_falls_back_to_ap() {
    WifiConfig c;
    TEST_ASSERT_EQUAL(WIFI_PARSE_DEFAULTED, parse("{\"wifi_function\":\"STA\",\"wifi_ssid\":\"Home\",\"wifi_pass\":\"short\",\"ap_pass\":\"\"}", c));
    TEST_ASSERT_EQUAL(WIFI_CFG_AP, c.mode);
}

static void test_parse_bad_ap_pass_uses_derived() {
    WifiConfig c;
    TEST_ASSERT_EQUAL(WIFI_PARSE_DEFAULTED, parse("{\"wifi_function\":\"AP\",\"wifi_ssid\":\"A\",\"wifi_pass\":\"\",\"ap_pass\":\"1234\"}", c));
    TEST_ASSERT_EQUAL_STRING("", c.ap_pass.c_str());
    TEST_ASSERT_EQUAL(WIFI_PARSE_DEFAULTED, parse("{\"wifi_function\":\"AP\",\"wifi_ssid\":\"A\",\"wifi_pass\":\"\",\"ap_pass\":12345678}", c));
    TEST_ASSERT_EQUAL_STRING("", c.ap_pass.c_str());
}

static void test_parse_malicious_ssid_rejected_for_ap() {
    WifiConfig c;
    // control character (\u0001) in SSID -> default SSID
    TEST_ASSERT_EQUAL(WIFI_PARSE_DEFAULTED, parse("{\"wifi_function\":\"AP\",\"wifi_ssid\":\"a\\u0001b\",\"ap_pass\":\"\"}", c));
    TEST_ASSERT_EQUAL_STRING("Voyager", c.ssid.c_str());
    // HTML in SSID is a valid 802.11 SSID; escaping is the UI's job
    TEST_ASSERT_EQUAL(WIFI_PARSE_OK, parse("{\"wifi_function\":\"AP\",\"wifi_ssid\":\"<img src=x>\",\"wifi_pass\":\"\",\"ap_pass\":\"\"}", c));
    TEST_ASSERT_EQUAL_STRING("<img src=x>", c.ssid.c_str());
}

static void test_valid_ssid() {
    TEST_ASSERT_FALSE(wifi_valid_ssid(nullptr));
    TEST_ASSERT_FALSE(wifi_valid_ssid(""));
    TEST_ASSERT_TRUE(wifi_valid_ssid("a"));
    TEST_ASSERT_TRUE(wifi_valid_ssid("01234567890123456789012345678901"));   // 32
    TEST_ASSERT_FALSE(wifi_valid_ssid("012345678901234567890123456789012")); // 33
    TEST_ASSERT_FALSE(wifi_valid_ssid("a\nb"));
    TEST_ASSERT_FALSE(wifi_valid_ssid("a\x7f"));
    TEST_ASSERT_TRUE(wifi_valid_ssid("caf\xc3\xa9"));  // UTF-8
}

static void test_valid_sta_pass() {
    TEST_ASSERT_FALSE(wifi_valid_sta_pass(nullptr));
    TEST_ASSERT_TRUE(wifi_valid_sta_pass(""));
    TEST_ASSERT_FALSE(wifi_valid_sta_pass("1234567"));
    TEST_ASSERT_TRUE(wifi_valid_sta_pass("12345678"));
    std::string p63(63, 'x'), p64(64, 'x');
    TEST_ASSERT_TRUE(wifi_valid_sta_pass(p63.c_str()));
    TEST_ASSERT_FALSE(wifi_valid_sta_pass(p64.c_str()));
    TEST_ASSERT_FALSE(wifi_valid_sta_pass("1234567\t8"));
}

static void test_valid_ap_pass() {
    TEST_ASSERT_FALSE(wifi_valid_ap_pass(nullptr));
    TEST_ASSERT_FALSE(wifi_valid_ap_pass(""));
    TEST_ASSERT_FALSE(wifi_valid_ap_pass("1234567"));
    TEST_ASSERT_TRUE(wifi_valid_ap_pass("12345678"));
    std::string p63(63, 'x'), p64(64, 'x');
    TEST_ASSERT_TRUE(wifi_valid_ap_pass(p63.c_str()));
    TEST_ASSERT_FALSE(wifi_valid_ap_pass(p64.c_str()));
}

static void test_ap_pass_setting_accepts_empty() {
    // Orchestrator clarification: "" resets to the derived default.
    TEST_ASSERT_TRUE(wifi_valid_ap_pass_setting(""));
    TEST_ASSERT_TRUE(wifi_valid_ap_pass_setting("12345678"));
    TEST_ASSERT_FALSE(wifi_valid_ap_pass_setting("1234567"));
    TEST_ASSERT_FALSE(wifi_valid_ap_pass_setting(std::string(64, 'x').c_str()));
    TEST_ASSERT_FALSE(wifi_valid_ap_pass_setting(nullptr));
    // and "" round-trips through the file format as "derived default"
    WifiConfig c; c.ap_pass = "";
    std::string j; wifi_serialize_config(c, j);
    WifiConfig back;
    TEST_ASSERT_EQUAL(WIFI_PARSE_OK, wifi_parse_config(j, back));
    TEST_ASSERT_EQUAL_STRING("", back.ap_pass.c_str());
}

static void test_derive_ap_pass() {
    const uint8_t mac[6] = {0x24, 0x6F, 0x28, 0xAB, 0x0c, 0x01};
    TEST_ASSERT_EQUAL_STRING("28AB0C01", wifi_derive_ap_pass(mac).c_str());
    const uint8_t z[6] = {0, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_STRING("00000000", wifi_derive_ap_pass(z).c_str());
    TEST_ASSERT_TRUE(wifi_valid_ap_pass(wifi_derive_ap_pass(mac).c_str()));
}

static void test_backoff() {
    TEST_ASSERT_EQUAL_UINT32(5000, wifi_next_backoff(0));
    TEST_ASSERT_EQUAL_UINT32(10000, wifi_next_backoff(5000));
    TEST_ASSERT_EQUAL_UINT32(20000, wifi_next_backoff(10000));
    TEST_ASSERT_EQUAL_UINT32(40000, wifi_next_backoff(20000));
    TEST_ASSERT_EQUAL_UINT32(60000, wifi_next_backoff(40000));
    TEST_ASSERT_EQUAL_UINT32(60000, wifi_next_backoff(60000));
    TEST_ASSERT_EQUAL_UINT32(60000, wifi_next_backoff(0xFFFFFFFFu));
}

static void test_serialize_roundtrip() {
    WifiConfig c;
    c.mode = WIFI_CFG_STA; c.ssid = "Home \"quoted\""; c.pass = "p@ss\\word1"; c.ap_pass = "apsecret1";
    std::string j; wifi_serialize_config(c, j);
    WifiConfig b;
    TEST_ASSERT_EQUAL(WIFI_PARSE_OK, wifi_parse_config(j, b));
    TEST_ASSERT_EQUAL(WIFI_CFG_STA, b.mode);
    TEST_ASSERT_EQUAL_STRING(c.ssid.c_str(), b.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING(c.pass.c_str(), b.pass.c_str());
    TEST_ASSERT_EQUAL_STRING(c.ap_pass.c_str(), b.ap_pass.c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_ap_ok);
    RUN_TEST(test_parse_sta_ok);
    RUN_TEST(test_parse_sta_open_network);
    RUN_TEST(test_parse_errors);
    RUN_TEST(test_parse_missing_keys_default);
    RUN_TEST(test_parse_unknown_mode_is_ap);
    RUN_TEST(test_parse_sta_bad_ssid_falls_back_to_ap);
    RUN_TEST(test_parse_sta_bad_pass_falls_back_to_ap);
    RUN_TEST(test_parse_bad_ap_pass_uses_derived);
    RUN_TEST(test_parse_malicious_ssid_rejected_for_ap);
    RUN_TEST(test_valid_ssid);
    RUN_TEST(test_valid_sta_pass);
    RUN_TEST(test_valid_ap_pass);
    RUN_TEST(test_ap_pass_setting_accepts_empty);
    RUN_TEST(test_derive_ap_pass);
    RUN_TEST(test_backoff);
    RUN_TEST(test_serialize_roundtrip);
    return UNITY_END();
}
