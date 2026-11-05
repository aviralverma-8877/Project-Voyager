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

// Bench scan (AP mode): TATA_3071 seen 4x (mesh), plus a hidden network.
static void test_scan_merge_dedupes_keeps_strongest() {
    WifiScanEntry l[8]; size_t n = 0;
    n = wifi_scan_merge(l, n, 8, "TATA_3071", -74);
    n = wifi_scan_merge(l, n, 8, "TATA_3071", -48);
    n = wifi_scan_merge(l, n, 8, "TATA_3071", -77);
    n = wifi_scan_merge(l, n, 8, "Hannah-Router", -84);
    n = wifi_scan_merge(l, n, 8, "", -60);          // hidden: skipped
    n = wifi_scan_merge(l, n, 8, nullptr, -60);     // null: skipped
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_STRING("TATA_3071", l[0].ssid);
    TEST_ASSERT_EQUAL(-48, l[0].rssi);
}

static void test_scan_merge_full_replaces_weakest() {
    WifiScanEntry l[2]; size_t n = 0;
    n = wifi_scan_merge(l, n, 2, "A", -90);
    n = wifi_scan_merge(l, n, 2, "B", -50);
    n = wifi_scan_merge(l, n, 2, "C", -95);   // weaker than all: dropped
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_STRING("A", l[0].ssid);
    n = wifi_scan_merge(l, n, 2, "D", -60);   // replaces A (-90)
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_STRING("D", l[0].ssid);
    TEST_ASSERT_EQUAL(-60, l[0].rssi);
    TEST_ASSERT_EQUAL(0, wifi_scan_merge(l, 0, 0, "X", -10));   // zero capacity
}

static void test_scan_merge_clamps_and_32_byte_ssid() {
    WifiScanEntry l[2]; size_t n = 0;
    const char* s32 = "0123456789abcdef0123456789abcdef";
    n = wifi_scan_merge(l, n, 2, s32, 5);          // positive rssi clamped to 0
    n = wifi_scan_merge(l, n, 2, "0123456789abcdef0123456789abcdefX", -40);  // 33 bytes: invalid
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_STRING(s32, l[0].ssid);
    TEST_ASSERT_EQUAL(0, l[0].rssi);
}

static void test_scan_sort_strongest_first() {
    WifiScanEntry l[4]; size_t n = 0;
    n = wifi_scan_merge(l, n, 4, "w", -90);
    n = wifi_scan_merge(l, n, 4, "s", -40);
    n = wifi_scan_merge(l, n, 4, "m", -70);
    n = wifi_scan_merge(l, n, 4, "m2", -70);
    wifi_scan_sort(l, n);
    TEST_ASSERT_EQUAL_STRING("s", l[0].ssid);
    TEST_ASSERT_EQUAL_STRING("m", l[1].ssid);    // stable for equal rssi
    TEST_ASSERT_EQUAL_STRING("m2", l[2].ssid);
    TEST_ASSERT_EQUAL_STRING("w", l[3].ssid);
    wifi_scan_sort(l, 0);
}

// --- strongest-BSSID selection (TATA_3071: 4 mesh nodes) ---
static const WifiApInfo kMesh[] = {
    {"TATA_3071", -74, {0xAA,1,1,1,1,1}, 6},
    {"Other",     -30, {0xBB,2,2,2,2,2}, 1},
    {"TATA_3071", -48, {0xAA,3,3,3,3,3}, 11},
    {"tata_3071", -20, {0xCC,4,4,4,4,4}, 1},   // case differs: not our network
    {"TATA_3071", -77, {0xAA,5,5,5,5,5}, 1},
    {"TATA_3071", -48, {0xAA,6,6,6,6,6}, 6},   // tie: first seen wins
};

static void test_pick_strongest_matching_bssid() {
    TEST_ASSERT_EQUAL(2, wifi_pick_strongest(kMesh, 6, "TATA_3071"));
    TEST_ASSERT_EQUAL(1, wifi_pick_strongest(kMesh, 6, "Other"));
    TEST_ASSERT_EQUAL(-1, wifi_pick_strongest(kMesh, 6, "Missing"));
    TEST_ASSERT_EQUAL(-1, wifi_pick_strongest(kMesh, 6, ""));
    TEST_ASSERT_EQUAL(-1, wifi_pick_strongest(nullptr, 0, "TATA_3071"));
}

static void test_pick_avoids_failed_node_unless_only_one() {
    const uint8_t failed[6] = {0xAA,3,3,3,3,3};
    TEST_ASSERT_EQUAL(5, wifi_pick_strongest(kMesh, 6, "TATA_3071", failed));   // next best -48
    TEST_ASSERT_EQUAL(1, wifi_pick_strongest(kMesh, 6, "Other", kMesh[1].bssid)); // only node: still used
    WifiApSelector one; one.set_avoid(kMesh[1].bssid);
    one.add("Other", "Other", -30, kMesh[1].bssid, 1);
    TEST_ASSERT_TRUE(one.only_avoided());                 // logged as "retrying it"
    TEST_ASSERT_TRUE(one.pick().valid);
    WifiApSelector two; two.set_avoid(failed);
    for (int i = 0; i < 6; i++) two.add("TATA_3071", kMesh[i].ssid, kMesh[i].rssi, kMesh[i].bssid, kMesh[i].channel);
    TEST_ASSERT_FALSE(two.only_avoided());
    TEST_ASSERT_EQUAL(4, two.matches);
}

static void test_consider_rejects_bad_channel_and_null() {
    WifiApCandidate b;
    const uint8_t m[6] = {1,2,3,4,5,6};
    TEST_ASSERT_FALSE(wifi_consider_ap(b, "X", "X", -50, m, 0));
    TEST_ASSERT_FALSE(wifi_consider_ap(b, "X", "X", -50, m, 15));
    TEST_ASSERT_FALSE(wifi_consider_ap(b, "X", "X", -50, nullptr, 6));
    TEST_ASSERT_FALSE(wifi_consider_ap(b, "X", nullptr, -50, m, 6));
    TEST_ASSERT_FALSE(b.valid);
    TEST_ASSERT_TRUE(wifi_consider_ap(b, "X", "X", -60, m, 14));
    TEST_ASSERT_FALSE(wifi_consider_ap(b, "X", "X", -60, m, 3));   // not better
    TEST_ASSERT_EQUAL(14, b.channel);
    WifiApSelector s;
    s.add("X", "X", -60, m, 1); s.add("X", "Y", -10, m, 1); s.add("X", "X", -70, m, 2);
    TEST_ASSERT_EQUAL(2, s.matches);
}

static void test_roam_decision() {
    WifiApCandidate best;
    const uint8_t other[6] = {9,9,9,9,9,9}, cur[6] = {1,1,1,1,1,1};
    TEST_ASSERT_FALSE(wifi_should_roam(-80, best, cur));          // nothing found
    wifi_consider_ap(best, "N", "N", -66, other, 6);
    TEST_ASSERT_FALSE(wifi_should_roam(-75, best, cur));          // 9 dB: marginal, stay
    TEST_ASSERT_TRUE(wifi_should_roam(-76, best, cur));           // 10 dB: roam
    TEST_ASSERT_FALSE(wifi_should_roam(-81, best, other));        // already on it
    TEST_ASSERT_FALSE(wifi_should_roam(0, best, cur));            // RSSI unknown
    TEST_ASSERT_FALSE(wifi_roam_check_due(-45, 600000, 600000));  // link good: no scan
    TEST_ASSERT_TRUE(wifi_roam_check_due(-55, 600000, 300000));   // far mesh node at -55: look
    TEST_ASSERT_FALSE(wifi_roam_check_due(-75, 30000, 600000));   // not settled
    TEST_ASSERT_FALSE(wifi_roam_check_due(-75, 600000, 60000));   // checked recently
    TEST_ASSERT_TRUE(wifi_roam_check_due(-75, 600000, 300000));
    TEST_ASSERT_FALSE(wifi_roam_check_due(0, 600000, 600000));
}

static void test_disconnect_reason_classes() {
    TEST_ASSERT_EQUAL(WIFI_DISC_BAD_AP, wifi_classify_reason(2));
    TEST_ASSERT_EQUAL(WIFI_DISC_BAD_AP, wifi_classify_reason(200));
    TEST_ASSERT_EQUAL(WIFI_DISC_BAD_AP, wifi_classify_reason(201));
    TEST_ASSERT_EQUAL(WIFI_DISC_BAD_AP, wifi_classify_reason(202));
    TEST_ASSERT_EQUAL(WIFI_DISC_AUTH, wifi_classify_reason(15));
    TEST_ASSERT_EQUAL(WIFI_DISC_AUTH, wifi_classify_reason(204));
    TEST_ASSERT_EQUAL(WIFI_DISC_SELF, wifi_classify_reason(8));
    TEST_ASSERT_EQUAL(WIFI_DISC_OTHER, wifi_classify_reason(1));
    TEST_ASSERT_EQUAL_STRING("AUTH_EXPIRE", wifi_reason_name(2));
    TEST_ASSERT_EQUAL_STRING("BEACON_TIMEOUT", wifi_reason_name(200));
    TEST_ASSERT_EQUAL_STRING("OTHER", wifi_reason_name(250));
}

static void test_format_bssid_and_hostname() {
    const uint8_t mac[6] = {0x54,0x43,0xB2,0x16,0xF1,0x5C};
    char b[18]; wifi_format_bssid(mac, b);
    TEST_ASSERT_EQUAL_STRING("54:43:B2:16:F1:5C", b);
    wifi_format_bssid(nullptr, b);
    TEST_ASSERT_EQUAL_STRING("", b);
    char h[WIFI_HOSTNAME_LEN]; wifi_format_hostname(mac, h);
    TEST_ASSERT_EQUAL_STRING("project-voyager-54-43-B2-16-F1-5C", h);
    for (const char* c = h; *c; c++)
        TEST_ASSERT_TRUE((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-');
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
    RUN_TEST(test_scan_merge_dedupes_keeps_strongest);
    RUN_TEST(test_scan_merge_full_replaces_weakest);
    RUN_TEST(test_scan_merge_clamps_and_32_byte_ssid);
    RUN_TEST(test_scan_sort_strongest_first);
    RUN_TEST(test_pick_strongest_matching_bssid);
    RUN_TEST(test_pick_avoids_failed_node_unless_only_one);
    RUN_TEST(test_consider_rejects_bad_channel_and_null);
    RUN_TEST(test_roam_decision);
    RUN_TEST(test_disconnect_reason_classes);
    RUN_TEST(test_format_bssid_and_hostname);
    return UNITY_END();
}
