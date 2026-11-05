// L2 tests for the LoRa network layer (src/lora/lora_proto.h): node ids, IPv4 ->
// node routing, T_NET framing and the SLIP codec used on the USB link.
#include <unity.h>
#include <string>
#include <vector>
#include "lora/lora_proto.h"

using namespace lora_proto;

void setUp() {}
void tearDown() {}

// Smallest valid IPv4 packet (20-byte header, no payload) to `dst`, total length n.
static std::vector<uint8_t> ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d, size_t n = 20) {
    std::vector<uint8_t> p(n, 0);
    p[0] = 0x45;
    p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
    p[8] = 64; p[9] = 6;                       // TTL, TCP
    p[12] = 10; p[13] = 73; p[14] = 1; p[15] = 2;
    p[16] = a; p[17] = b; p[18] = c; p[19] = d;
    return p;
}

static void test_node_id_from_mac() {
    const uint8_t a[6] = {0x54, 0x43, 0xB2, 0x16, 0xE8, 0xEC};   // board .37
    const uint8_t b[6] = {0x54, 0x43, 0xB2, 0x16, 0xF1, 0x5C};   // board .38
    TEST_ASSERT_EQUAL_HEX16(0xE8EC, net_node_id(a));
    TEST_ASSERT_EQUAL_HEX16(0xF15C, net_node_id(b));
    const uint8_t edge[6] = {0, 0, 0, 0, 0x00, 0xFF};
    TEST_ASSERT_EQUAL_HEX16(0x01FE, net_node_id(edge));           // .0/.255 avoided
}

static void test_ip_to_node() {
    const uint8_t uni[4] = {10, 73, 0xE8, 0xEC};
    const uint8_t bcast[4] = {10, 73, 255, 255};
    const uint8_t all[4] = {255, 255, 255, 255};
    const uint8_t other[4] = {192, 168, 1, 37};
    const uint8_t net0[4] = {10, 73, 0, 5};
    const uint8_t host255[4] = {10, 73, 5, 255};
    TEST_ASSERT_EQUAL_HEX16(0xE8EC, net_ip_to_node(uni));
    TEST_ASSERT_EQUAL_HEX16(NET_BROADCAST, net_ip_to_node(bcast));
    TEST_ASSERT_EQUAL_HEX16(NET_BROADCAST, net_ip_to_node(all));
    TEST_ASSERT_EQUAL_HEX16(0, net_ip_to_node(other));
    TEST_ASSERT_EQUAL_HEX16(0, net_ip_to_node(net0));
    TEST_ASSERT_EQUAL_HEX16(0, net_ip_to_node(host255));
}

static void test_ipv4_check() {
    auto p = ipv4(10, 73, 1, 1, 60);
    TEST_ASSERT_TRUE(net_ipv4_ok(p.data(), p.size()));
    TEST_ASSERT_FALSE(net_ipv4_ok(p.data(), p.size() - 1));       // length mismatch
    p[0] = 0x60;                                                   // IPv6
    TEST_ASSERT_FALSE(net_ipv4_ok(p.data(), p.size()));
    p[0] = 0x44;                                                   // IHL 16 < 20
    TEST_ASSERT_FALSE(net_ipv4_ok(p.data(), p.size()));
    TEST_ASSERT_FALSE(net_ipv4_ok(p.data(), 10));
}

static void test_net_frame_round_trip_and_limits() {
    auto ip = ipv4(10, 73, 0xF1, 0x5C, 100);
    uint8_t buf[MAX_PAYLOAD];
    size_t n = net_build(0xF15C, 0xE8EC, ip.data(), ip.size(), buf, sizeof buf);
    TEST_ASSERT_EQUAL_size_t(NET_HDR + 100, n);
    NetFrame f;
    TEST_ASSERT_TRUE(net_parse(buf, n, f));
    TEST_ASSERT_EQUAL_HEX16(0xF15C, f.dst);
    TEST_ASSERT_EQUAL_HEX16(0xE8EC, f.src);
    TEST_ASSERT_EQUAL_size_t(100, f.len);
    TEST_ASSERT_EQUAL_MEMORY(ip.data(), f.ip, 100);

    // The largest packet fills one LoRa frame exactly.
    auto big = ipv4(10, 73, 1, 1, NET_MAX_IP);
    n = net_build(0x0101, 0x0202, big.data(), big.size(), buf, sizeof buf);
    TEST_ASSERT_EQUAL_size_t(MAX_PAYLOAD, n);
    uint8_t frame[MAX_FRAME];
    TEST_ASSERT_TRUE(frame_packet(T_NET, 9, buf, n, frame, sizeof frame) > 0);
    Parsed pp = parse_packet(frame, HEADER_LEN + n);
    TEST_ASSERT_TRUE(pp.ok);
    TEST_ASSERT_EQUAL_UINT8(T_NET, pp.type);

    auto too_big = ipv4(10, 73, 1, 1, NET_MAX_IP + 1);
    TEST_ASSERT_EQUAL_size_t(0, net_build(1, 2, too_big.data(), too_big.size(), buf, sizeof buf));

    // Bad source / not IPv4 are rejected.
    n = net_build(0xF15C, NET_BROADCAST, ip.data(), ip.size(), buf, sizeof buf);
    TEST_ASSERT_FALSE(net_parse(buf, n, f));
    n = net_build(0xF15C, 0xE8EC, ip.data(), ip.size(), buf, sizeof buf);
    buf[NET_HDR] = 0x60;
    TEST_ASSERT_FALSE(net_parse(buf, n, f));
}

static std::vector<uint8_t> slip_roundtrip(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> enc(2 * in.size() + 2);
    size_t n = slip_encode(in.data(), in.size(), enc.data(), enc.size());
    TEST_ASSERT_TRUE(n >= in.size() + 2);
    SlipDecoder<512> d;
    std::vector<uint8_t> out;
    for (size_t i = 0; i < n; i++) {
        size_t m = d.feed(enc[i]);
        if (m) out.assign(d.frame(), d.frame() + m);
    }
    return out;
}

static void test_slip_round_trip_all_bytes() {
    std::vector<uint8_t> all;
    for (int i = 0; i < 256; i++) all.push_back((uint8_t)i);
    all.insert(all.end(), {SLIP_END, SLIP_END, SLIP_ESC, SLIP_ESC_END, SLIP_ESC});
    TEST_ASSERT_TRUE(slip_roundtrip(all) == all);
}

static void test_slip_wire_format() {
    const uint8_t in[] = {0x01, SLIP_END, 0x02, SLIP_ESC};
    uint8_t out[16];
    size_t n = slip_encode(in, sizeof in, out, sizeof out);
    const uint8_t want[] = {SLIP_END, 0x01, SLIP_ESC, SLIP_ESC_END, 0x02, SLIP_ESC, SLIP_ESC_ESC, SLIP_END};
    TEST_ASSERT_EQUAL_size_t(sizeof want, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, n);
    TEST_ASSERT_EQUAL_size_t(0, slip_encode(in, sizeof in, out, 5));   // too small
}

static void test_slip_decoder_drops_bad_frames() {
    SlipDecoder<8> d;
    // empty frames (back-to-back END) yield nothing
    TEST_ASSERT_EQUAL_size_t(0, d.feed(SLIP_END));
    TEST_ASSERT_EQUAL_size_t(0, d.feed(SLIP_END));
    // bad escape: dropped, next frame fine
    d.feed(0x11); d.feed(SLIP_ESC); d.feed(0x22);
    TEST_ASSERT_EQUAL_size_t(0, d.feed(SLIP_END));
    d.feed(0x33);
    TEST_ASSERT_EQUAL_size_t(1, d.feed(SLIP_END));
    TEST_ASSERT_EQUAL_HEX8(0x33, d.frame()[0]);
    // oversized: dropped
    for (int i = 0; i < 9; i++) d.feed(0x44);
    TEST_ASSERT_EQUAL_size_t(0, d.feed(SLIP_END));
    d.feed(0x55);
    TEST_ASSERT_EQUAL_size_t(1, d.feed(SLIP_END));
}

static void test_send_to_control_frame() {
    auto ip = ipv4(142, 250, 1, 1, 60);              // internet destination
    std::vector<uint8_t> f = {0x00, NET_CTL_SEND_TO, 0xE8, 0xEC};
    f.insert(f.end(), ip.begin(), ip.end());
    uint16_t node; const uint8_t* p; size_t len;
    TEST_ASSERT_TRUE(net_parse_send_to(f.data(), f.size(), node, p, len));
    TEST_ASSERT_EQUAL_HEX16(0xE8EC, node);
    TEST_ASSERT_EQUAL_size_t(60, len);
    TEST_ASSERT_EQUAL_HEX8(142, p[16]);

    f[2] = 0xFF; f[3] = 0xFF;                        // broadcast is not a gateway
    TEST_ASSERT_FALSE(net_parse_send_to(f.data(), f.size(), node, p, len));
    f[2] = 0; f[3] = 0;
    TEST_ASSERT_FALSE(net_parse_send_to(f.data(), f.size(), node, p, len));
    f[2] = 0xE8; f[3] = 0xEC;
    TEST_ASSERT_FALSE(net_parse_send_to(f.data(), f.size() - 1, node, p, len));   // length mismatch
    const uint8_t keepalive[] = {0x00, NET_CTL_KEEPALIVE};
    TEST_ASSERT_FALSE(net_parse_send_to(keepalive, sizeof keepalive, node, p, len));
}

static void test_dedup_key_tagged() {
    TEST_ASSERT_EQUAL_HEX32(0x4E45F15Cu, net_dedup_key(0xF15C));
    TEST_ASSERT_NOT_EQUAL(0u, net_dedup_key(0));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_node_id_from_mac);
    RUN_TEST(test_ip_to_node);
    RUN_TEST(test_ipv4_check);
    RUN_TEST(test_net_frame_round_trip_and_limits);
    RUN_TEST(test_slip_round_trip_all_bytes);
    RUN_TEST(test_slip_wire_format);
    RUN_TEST(test_slip_decoder_drops_bad_frames);
    RUN_TEST(test_send_to_control_frame);
    RUN_TEST(test_dedup_key_tagged);
    return UNITY_END();
}
