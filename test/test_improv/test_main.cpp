// L2 tests for the Improv Serial framing (src/core/improv.h) used for WiFi setup
// from the web flasher.
#include <unity.h>
#include <string>
#include <vector>
#include "core/improv.h"

using namespace improv;

void setUp() {}
void tearDown() {}

static std::vector<uint8_t> bytes(const uint8_t* p, size_t n) { return std::vector<uint8_t>(p, p + n); }

static int feed_all(Parser& ps, const std::vector<uint8_t>& in, Packet& out, int* last = nullptr) {
    int got = 0;
    for (uint8_t b : in) {
        int r = ps.feed(b, out);
        if (r != 0) { got++; if (last) *last = r; }
    }
    return got;
}

// Hand-built from the spec: state "Ready" (0x02).
// 'I'+'M'+'P'+'R'+'O'+'V' = 73+77+80+82+79+86 = 477 (0x1DD),
// +1 (version) +1 (type) +1 (len) +2 (data) = 482 (0x1E2) -> 0xE2
static void test_state_packet_bytes() {
    uint8_t out[32];
    size_t n = build_state(STATE_READY, out, sizeof out);
    const uint8_t want[] = {'I', 'M', 'P', 'R', 'O', 'V', 0x01, 0x01, 0x01, 0x02, 0xE2, '\n'};
    TEST_ASSERT_EQUAL_size_t(sizeof want, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, n);
}

static void test_round_trip_and_noise() {
    uint8_t pkt[64];
    const uint8_t data[] = {CMD_GET_DEVICE_INFO, 0x00};
    size_t n = build(TYPE_RPC, data, sizeof data, pkt, sizeof pkt);
    std::vector<uint8_t> in = {'b', 'o', 'o', 't', '\r', '\n', 'I', 'I', 'M'};  // log noise, false start
    auto p = bytes(pkt, n);
    in.insert(in.end(), p.begin(), p.end());
    Parser ps;
    Packet got;
    int last = 0;
    TEST_ASSERT_EQUAL_INT(1, feed_all(ps, in, got, &last));
    TEST_ASSERT_EQUAL_INT(1, last);
    uint8_t cmd; const uint8_t* pl; size_t plen;
    TEST_ASSERT_TRUE(rpc_command(got, cmd, pl, plen));
    TEST_ASSERT_EQUAL_UINT8(CMD_GET_DEVICE_INFO, cmd);
    TEST_ASSERT_EQUAL_size_t(0, plen);
}

static void test_bad_checksum_reported() {
    uint8_t pkt[32];
    size_t n = build_state(STATE_READY, pkt, sizeof pkt);
    pkt[n - 2] ^= 0x01;  // checksum (before '\n')
    Parser ps;
    Packet got;
    int last = 0;
    TEST_ASSERT_EQUAL_INT(1, feed_all(ps, bytes(pkt, n), got, &last));
    TEST_ASSERT_EQUAL_INT(-1, last);
    // and the parser recovers for the next good packet
    n = build_state(STATE_READY, pkt, sizeof pkt);
    TEST_ASSERT_EQUAL_INT(1, feed_all(ps, bytes(pkt, n), got, &last));
    TEST_ASSERT_EQUAL_INT(1, last);
}

static std::vector<uint8_t> wifi_rpc(const std::string& ssid, const std::string& pass) {
    std::vector<uint8_t> d = {CMD_WIFI_SETTINGS, (uint8_t)(ssid.size() + pass.size() + 2), (uint8_t)ssid.size()};
    d.insert(d.end(), ssid.begin(), ssid.end());
    d.push_back((uint8_t)pass.size());
    d.insert(d.end(), pass.begin(), pass.end());
    return d;
}

static void test_wifi_settings_parse() {
    auto d = wifi_rpc("TATA_3071", "secret-pass");
    uint8_t pkt[128];
    size_t n = build(TYPE_RPC, d.data(), d.size(), pkt, sizeof pkt);
    Parser ps;
    Packet got;
    TEST_ASSERT_EQUAL_INT(1, feed_all(ps, bytes(pkt, n), got));
    uint8_t cmd; const uint8_t* pl; size_t plen;
    TEST_ASSERT_TRUE(rpc_command(got, cmd, pl, plen));
    TEST_ASSERT_EQUAL_UINT8(CMD_WIFI_SETTINGS, cmd);
    char ssid[33], pass[65];
    TEST_ASSERT_TRUE(parse_wifi_settings(pl, plen, ssid, pass));
    TEST_ASSERT_EQUAL_STRING("TATA_3071", ssid);
    TEST_ASSERT_EQUAL_STRING("secret-pass", pass);

    // open network: empty password
    d = wifi_rpc("Cafe", "");
    TEST_ASSERT_TRUE(parse_wifi_settings(d.data() + 2, d[1], ssid, pass));
    TEST_ASSERT_EQUAL_STRING("", pass);
}

static void test_wifi_settings_rejects_malformed() {
    char ssid[33], pass[65];
    auto d = wifi_rpc("", "x");                       // empty SSID
    TEST_ASSERT_FALSE(parse_wifi_settings(d.data() + 2, d[1], ssid, pass));
    d = wifi_rpc(std::string(33, 'a'), "");           // SSID too long
    TEST_ASSERT_FALSE(parse_wifi_settings(d.data() + 2, d[1], ssid, pass));
    d = wifi_rpc("net", "password");
    TEST_ASSERT_FALSE(parse_wifi_settings(d.data() + 2, d[1] - 3, ssid, pass));  // truncated
    const uint8_t lies[] = {5, 'a', 'b'};             // claims 5 bytes, has 2
    TEST_ASSERT_FALSE(parse_wifi_settings(lies, sizeof lies, ssid, pass));

    Packet p;                                          // RPC whose length overruns the packet
    p.type = TYPE_RPC; p.len = 2; p.data[0] = CMD_WIFI_SETTINGS; p.data[1] = 9;
    uint8_t cmd; const uint8_t* pl; size_t plen;
    TEST_ASSERT_FALSE(rpc_command(p, cmd, pl, plen));
}

static void test_rpc_result_layout() {
    const char* strs[] = {"http://192.168.1.38"};
    uint8_t out[64];
    size_t n = build_rpc_result(CMD_WIFI_SETTINGS, strs, 1, out, sizeof out);
    TEST_ASSERT_TRUE(n > 0);
    Parser ps;
    Packet got;
    TEST_ASSERT_EQUAL_INT(1, feed_all(ps, bytes(out, n), got));
    TEST_ASSERT_EQUAL_UINT8(TYPE_RPC_RESULT, got.type);
    TEST_ASSERT_EQUAL_UINT8(CMD_WIFI_SETTINGS, got.data[0]);
    TEST_ASSERT_EQUAL_UINT8(1 + 19, got.data[1]);       // one length byte + the URL
    TEST_ASSERT_EQUAL_UINT8(19, got.data[2]);
    TEST_ASSERT_EQUAL_MEMORY("http://192.168.1.38", got.data + 3, 19);

    // empty result (end of a scan list)
    n = build_rpc_result(CMD_GET_WIFI_NETWORKS, nullptr, 0, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(1, feed_all(ps, bytes(out, n), got));
    TEST_ASSERT_EQUAL_UINT8(2, got.len);
    TEST_ASSERT_EQUAL_UINT8(0, got.data[1]);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_state_packet_bytes);
    RUN_TEST(test_round_trip_and_noise);
    RUN_TEST(test_bad_checksum_reported);
    RUN_TEST(test_wifi_settings_parse);
    RUN_TEST(test_wifi_settings_rejects_malformed);
    RUN_TEST(test_rpc_result_layout);
    return UNITY_END();
}
