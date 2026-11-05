// L2 native tests for the LoRa v2 protocol helpers (src/lora_proto.h).
// Run: ~/.platformio/penv/bin/pio test -e native -f test_lora
#include <unity.h>
#include <string.h>
#include <string>
#include "lora/lora_proto.h"

using namespace lora_proto;

void setUp() {}
void tearDown() {}

// ---- CRC8 ---------------------------------------------------------------------
static void test_crc8_known_vector() {
    // CRC-8 (poly 0x07, init 0, no reflect, no xorout) of "123456789" is 0xF4.
    const char* s = "123456789";
    TEST_ASSERT_EQUAL_HEX8(0xF4, crc8((const uint8_t*)s, 9));
    TEST_ASSERT_EQUAL_HEX8(0x00, crc8(nullptr, 0));
}

static void test_packet_crc_covers_seq() {
    const uint8_t p[] = {'h', 'i'};
    TEST_ASSERT_NOT_EQUAL(packet_crc(1, p, 2), packet_crc(2, p, 2));
    uint8_t joined[] = {7, 'h', 'i'};
    TEST_ASSERT_EQUAL_HEX8(crc8(joined, 3), packet_crc(7, p, 2));
}

// ---- framing round trip ------------------------------------------------------
static void roundtrip_len(size_t len) {
    uint8_t payload[260];
    for (size_t i = 0; i < len; i++) payload[i] = (uint8_t)(i * 31 + 7);
    uint8_t frame[MAX_FRAME];
    size_t n = frame_packet(T_RAW_DATA, 0xAB, payload, len, frame, sizeof frame);
    TEST_ASSERT_EQUAL_UINT(HEADER_LEN + len, n);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)len, frame[0]);
    TEST_ASSERT_EQUAL_UINT8(T_RAW_DATA, frame[1]);
    TEST_ASSERT_EQUAL_UINT8(0xAB, frame[2]);
    Parsed p = parse_packet(frame, n);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL_UINT8(T_RAW_DATA, p.type);
    TEST_ASSERT_EQUAL_UINT8(0xAB, p.seq);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)len, p.len);
    if (len) TEST_ASSERT_EQUAL_MEMORY(payload, p.payload, len);
}
static void test_roundtrip_len0() { roundtrip_len(0); }
static void test_roundtrip_len1() { roundtrip_len(1); }
static void test_roundtrip_len251() { roundtrip_len(251); }

static void test_reject_len252() {
    uint8_t payload[252] = {0};
    uint8_t frame[300];
    TEST_ASSERT_EQUAL_UINT(0, frame_packet(T_LORA_MSG, 1, payload, 252, frame, sizeof frame));
    TEST_ASSERT_EQUAL_UINT(251, MAX_PAYLOAD);
}

static void test_frame_rejects_small_buffer_and_ack_type() {
    uint8_t payload[10] = {0};
    uint8_t frame[8];
    TEST_ASSERT_EQUAL_UINT(0, frame_packet(T_LORA_MSG, 1, payload, 10, frame, sizeof frame));
    uint8_t big[32];
    TEST_ASSERT_EQUAL_UINT(0, frame_packet(T_REC_AKNG, 1, payload, 1, big, sizeof big));
    TEST_ASSERT_EQUAL_UINT(0, frame_packet(9, 1, payload, 1, big, sizeof big));
}

static void test_serial_frame_roundtrip() {
    const char* s = "AT+X";
    uint8_t f[16];
    size_t n = frame_packet(T_LORA_SERIAL, 200, (const uint8_t*)s, 4, f, sizeof f);
    Parsed p = parse_packet(f, n);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL_UINT8(T_LORA_SERIAL, p.type);
    TEST_ASSERT_EQUAL_UINT8(200, p.seq);
}

// ---- parse errors -------------------------------------------------------------
static void test_bad_crc() {
    const char* s = "hello";
    uint8_t f[16];
    size_t n = frame_packet(T_LORA_MSG, 5, (const uint8_t*)s, 5, f, sizeof f);
    f[HEADER_LEN + 2] ^= 0x01;  // corrupt payload
    Parsed p = parse_packet(f, n);
    TEST_ASSERT_FALSE(p.ok);
    TEST_ASSERT_EQUAL_INT((int)ParseErr::BadCrc, (int)p.err);

    n = frame_packet(T_LORA_MSG, 5, (const uint8_t*)s, 5, f, sizeof f);
    f[2] = 6;  // corrupt seq: CRC covers it
    p = parse_packet(f, n);
    TEST_ASSERT_FALSE(p.ok);
    TEST_ASSERT_EQUAL_INT((int)ParseErr::BadCrc, (int)p.err);
}

static void test_len_mismatch_and_short() {
    const char* s = "abc";
    uint8_t f[16];
    size_t n = frame_packet(T_LORA_MSG, 5, (const uint8_t*)s, 3, f, sizeof f);
    TEST_ASSERT_EQUAL_INT((int)ParseErr::LenMismatch, (int)parse_packet(f, n - 1).err);
    TEST_ASSERT_EQUAL_INT((int)ParseErr::LenMismatch, (int)parse_packet(f, n + 1).err);
    TEST_ASSERT_EQUAL_INT((int)ParseErr::TooShort, (int)parse_packet(f, 3).err);
    f[1] = 4;
    TEST_ASSERT_EQUAL_INT((int)ParseErr::BadType, (int)parse_packet(f, n).err);
}

// ---- ACK shape ------------------------------------------------------------------
static void test_ack_shape_and_seq_echo() {
    uint8_t f[8];
    size_t n = frame_ack(0x9C, ACK_OK, f, sizeof f);
    TEST_ASSERT_EQUAL_UINT(5, n);
    const uint8_t expect[] = {1, T_REC_AKNG, 0x9C, 0, 1};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, f, 5);
    Parsed p = parse_packet(f, n);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL_UINT8(T_REC_AKNG, p.type);
    TEST_ASSERT_EQUAL_UINT8(0x9C, p.seq);
    TEST_ASSERT_EQUAL_UINT8(1, p.payload[0]);

    // receiver echoes the seq of the packet it got
    const char* s = "x";
    uint8_t d[8];
    size_t dn = frame_packet(T_LORA_MSG, 77, (const uint8_t*)s, 1, d, sizeof d);
    Parsed dp = parse_packet(d, dn);
    n = frame_ack(dp.seq, ACK_RETRY, f, sizeof f);
    TEST_ASSERT_EQUAL_UINT8(77, f[2]);
    TEST_ASSERT_EQUAL_UINT8(0, f[4]);
}

static void test_ack_wrong_len_rejected() {
    const uint8_t f[] = {2, T_REC_AKNG, 1, 0, 1, 1};
    Parsed p = parse_packet(f, sizeof f);
    TEST_ASSERT_FALSE(p.ok);
    TEST_ASSERT_EQUAL_INT((int)ParseErr::BadAck, (int)p.err);
}

// ---- retry state machine ---------------------------------------------------------
static void test_tx_ack_ok() {
    TxRetry t;
    tx_begin(t, 10);
    TEST_ASSERT_EQUAL_INT((int)TxAction::Success, (int)tx_on_ack(t, 10, 1));
    TEST_ASSERT_TRUE(t.done);
    TEST_ASSERT_EQUAL_UINT8(1, t.attempts);
}

static void test_tx_mismatched_seq_ignored() {
    TxRetry t;
    tx_begin(t, 10);
    TEST_ASSERT_EQUAL_INT((int)TxAction::Wait, (int)tx_on_ack(t, 9, 1));
    TEST_ASSERT_EQUAL_INT((int)TxAction::Wait, (int)tx_on_ack(t, 11, 0));
    TEST_ASSERT_FALSE(t.done);
    TEST_ASSERT_EQUAL_UINT8(1, t.attempts);
    TEST_ASSERT_EQUAL_INT((int)TxAction::Success, (int)tx_on_ack(t, 10, 1));
}

static void test_tx_nack_and_odd_values_retry() {
    TxRetry t;
    tx_begin(t, 3);
    TEST_ASSERT_EQUAL_INT((int)TxAction::Retransmit, (int)tx_on_ack(t, 3, 0));
    TEST_ASSERT_EQUAL_INT((int)TxAction::Retransmit, (int)tx_on_ack(t, 3, 2));
    TEST_ASSERT_EQUAL_INT((int)TxAction::Retransmit, (int)tx_on_ack(t, 3, 0xFF));
    TEST_ASSERT_EQUAL_UINT8(4, t.attempts);
    TEST_ASSERT_EQUAL_UINT8(3, t.seq);  // retries reuse seq
    TEST_ASSERT_EQUAL_INT((int)TxAction::Success, (int)tx_on_ack(t, 3, 1));
}

static void test_tx_retry_exhaustion() {
    TxRetry t;
    tx_begin(t, 200);
    for (int i = 0; i < MAX_RETRIES; i++)
        TEST_ASSERT_EQUAL_INT((int)TxAction::Retransmit, (int)tx_on_timeout(t));
    TEST_ASSERT_EQUAL_UINT8(1 + MAX_RETRIES, t.attempts);
    TEST_ASSERT_EQUAL_INT((int)TxAction::Fail, (int)tx_on_timeout(t));
    TEST_ASSERT_TRUE(t.done);
    // late ACK after failure changes nothing
    TEST_ASSERT_EQUAL_INT((int)TxAction::Wait, (int)tx_on_ack(t, 200, 1));
}

static void test_tx_mixed_nack_timeout_exhaustion() {
    TxRetry t;
    tx_begin(t, 1);
    TEST_ASSERT_EQUAL_INT((int)TxAction::Retransmit, (int)tx_on_ack(t, 1, 0));
    TEST_ASSERT_EQUAL_INT((int)TxAction::Retransmit, (int)tx_on_timeout(t));
    TEST_ASSERT_EQUAL_INT((int)TxAction::Retransmit, (int)tx_on_ack(t, 1, 7));
    TEST_ASSERT_EQUAL_INT((int)TxAction::Fail, (int)tx_on_ack(t, 1, 0));
}

// ---- dedup -------------------------------------------------------------------------
static void test_dedup() {
    RxDedup d;
    TEST_ASSERT_FALSE(rx_is_duplicate(d, 42));  // first packet ever
    rx_commit(d, 42);
    TEST_ASSERT_TRUE(rx_is_duplicate(d, 42));   // retry of the same packet: ACK 1, don't deliver
    TEST_ASSERT_TRUE(rx_is_duplicate(d, 42));   // re-ACKed every time
    TEST_ASSERT_FALSE(rx_is_duplicate(d, 43));  // new seq accepted
    rx_commit(d, 43);
    TEST_ASSERT_TRUE(rx_is_duplicate(d, 43));
    TEST_ASSERT_FALSE(rx_is_duplicate(d, 42));
}

static void test_dedup_seq_wraps() {
    RxDedup d;
    rx_commit(d, 255);
    TEST_ASSERT_FALSE(rx_is_duplicate(d, (uint8_t)(255 + 1)));
}

static void test_dedup_uncommitted_not_duplicate() {
    // packet dropped (queue full, NACKed) is not committed, so its retry is delivered
    RxDedup d;
    rx_commit(d, 5);
    TEST_ASSERT_FALSE(rx_is_duplicate(d, 6));
    TEST_ASSERT_FALSE(rx_is_duplicate(d, 6));
}

// ---- per-source dedup (multi-peer) ------------------------------------------------
static void test_dedup_multi_basic() {
    RxDedupMulti d;
    uint32_t mac1 = hash_mac("AABBCCDD");
    uint32_t mac2 = hash_mac("11223344");
    TEST_ASSERT_FALSE(rx_is_duplicate_multi(d, mac1, 42));  // first packet from peer 1
    rx_commit_multi(d, mac1, 42);
    TEST_ASSERT_TRUE(rx_is_duplicate_multi(d, mac1, 42));   // retry from peer 1
    TEST_ASSERT_FALSE(rx_is_duplicate_multi(d, mac2, 42));  // same seq, different peer
    rx_commit_multi(d, mac2, 42);
    TEST_ASSERT_TRUE(rx_is_duplicate_multi(d, mac1, 42));   // peer 1 still tracked
    TEST_ASSERT_TRUE(rx_is_duplicate_multi(d, mac2, 42));   // peer 2 now tracked
}

static void test_dedup_multi_new_seq_accepted() {
    RxDedupMulti d;
    uint32_t mac1 = hash_mac("AABBCCDD");
    rx_commit_multi(d, mac1, 10);
    TEST_ASSERT_FALSE(rx_is_duplicate_multi(d, mac1, 11));  // new seq from same peer
    rx_commit_multi(d, mac1, 11);
    TEST_ASSERT_FALSE(rx_is_duplicate_multi(d, mac1, 10));  // old seq no longer tracked
}

static void test_dedup_multi_lru_eviction() {
    RxDedupMulti d;
    // Fill all DEDUP_SLOTS
    for (size_t i = 0; i < DEDUP_SLOTS; ++i) {
        rx_commit_multi(d, (uint32_t)(i + 1), (uint8_t)(i * 10));
    }
    // New peer evicts oldest
    uint32_t new_mac = (uint32_t)(DEDUP_SLOTS + 1);
    TEST_ASSERT_FALSE(rx_is_duplicate_multi(d, new_mac, 99));
    rx_commit_multi(d, new_mac, 99);
    TEST_ASSERT_TRUE(rx_is_duplicate_multi(d, new_mac, 99));
    // Slot 0 (key 1) was evicted
    TEST_ASSERT_FALSE(rx_is_duplicate_multi(d, 1, 0));
}

static void test_dedup_multi_peers_independent() {
    RxDedupMulti d;
    uint32_t a = hash_mac("54:43:B2:16:E8:EC"), b = hash_mac("54:43:B2:16:F1:5C");
    TEST_ASSERT_FALSE(rx_is_duplicate_multi(d, a, 50));
    rx_commit_multi(d, a, 50);
    TEST_ASSERT_TRUE(rx_is_duplicate_multi(d, a, 50));
    // Same seq from a different peer doesn't collide
    TEST_ASSERT_FALSE(rx_is_duplicate_multi(d, b, 50));
}

// Regression: peer A sends seq s, then 255 packets from peer B are accepted, then A
// sends again with seq s+256 == s. Aging (DEDUP_WINDOW) must keep A's new message
// from being dropped as a duplicate of the stale entry.
static void test_dedup_multi_seq_wrap_across_types() {
    RxDedupMulti d;
    uint32_t mac = hash_mac("54:43:B2:16:F1:5C");
    uint32_t other = hash_mac("54:43:B2:16:E8:EC");
    uint8_t s = 200;
    TEST_ASSERT_FALSE(rx_is_duplicate_multi(d, mac, s));
    rx_commit_multi(d, mac, s);
    for (int i = 1; i <= 255; ++i) {
        uint8_t q = (uint8_t)(s + i);
        TEST_ASSERT_FALSE(rx_is_duplicate_multi(d, other, q));
        rx_commit_multi(d, other, q);
    }
    TEST_ASSERT_FALSE(rx_is_duplicate_multi(d, mac, (uint8_t)(s + 256)));
}

// A retry is still caught when a few packets from other peers came in between.
static void test_dedup_multi_retry_after_interleave() {
    RxDedupMulti d;
    uint32_t a = hash_mac("A"), b = hash_mac("B");
    rx_commit_multi(d, a, 7);
    for (int i = 0; i < 5; ++i) rx_commit_multi(d, b, (uint8_t)(100 + i));
    TEST_ASSERT_TRUE(rx_is_duplicate_multi(d, a, 7));
    // ...but not after DEDUP_WINDOW other accepted packets.
    for (uint32_t i = 0; i < DEDUP_WINDOW; ++i) rx_commit_multi(d, b, (uint8_t)(110 + i));
    TEST_ASSERT_FALSE(rx_is_duplicate_multi(d, a, 7));
}

// A check that's never committed (queue full -> NACK) must not evict anything.
static void test_dedup_multi_check_has_no_side_effect() {
    RxDedupMulti d;
    for (size_t i = 0; i < DEDUP_SLOTS; ++i) rx_commit_multi(d, (uint32_t)(i + 1), (uint8_t)i);
    for (int k = 0; k < 10; ++k) TEST_ASSERT_FALSE(rx_is_duplicate_multi(d, 999u + k, 1));
    for (size_t i = 0; i < DEDUP_SLOTS; ++i)
        TEST_ASSERT_TRUE(rx_is_duplicate_multi(d, (uint32_t)(i + 1), (uint8_t)i));
}

// Time on air vs the Semtech calculator (explicit header, CRC on, preamble 8).
static void test_airtime() {
    // SF8 / BW500k / 4/7, 116-byte payload + 4 header = 120 bytes -> ~121 ms
    uint32_t t = airtime_ms(8, 500000, 7, 120);
    TEST_ASSERT_UINT32_WITHIN(5, 121, t);
    // SF7 / BW125k / 4/5, 10 bytes -> 41.2 ms
    TEST_ASSERT_UINT32_WITHIN(2, 42, airtime_ms(7, 125000, 5, 10));
    // SF12 / BW125k / 4/5, 10 bytes (LDRO on) -> 991.2 ms
    TEST_ASSERT_UINT32_WITHIN(5, 992, airtime_ms(12, 125000, 5, 10));
    // Slowest allowed config: an ACK frame (5 bytes) alone takes about 14.8 s.
    uint32_t ack = airtime_ms(12, 7800, 8, 5);
    TEST_ASSERT_TRUE(ack > 14000 && ack < 16000);
    // Monotonic in payload length; invalid input -> 0.
    TEST_ASSERT_TRUE(airtime_ms(8, 125000, 7, 255) > airtime_ms(8, 125000, 7, 5));
    TEST_ASSERT_EQUAL_UINT32(0, airtime_ms(5, 125000, 5, 10));
    TEST_ASSERT_EQUAL_UINT32(0, airtime_ms(7, 0, 5, 10));
    TEST_ASSERT_EQUAL_UINT32(0, airtime_ms(7, 125000, 9, 10));
}

// Contract 3 tx_started.expected_ms
static void test_tx_expected_ms() {
    TEST_ASSERT_EQUAL_UINT8(4, TX_ATTEMPTS);
    // Formula check against its parts.
    const uint32_t data = airtime_ms(8, 500000, 7, 119), ack = airtime_ms(8, 500000, 7, ACK_FRAME_LEN);
    TEST_ASSERT_EQUAL_UINT32(4u * (data + 5000 + ack) + 3u * (TX_RETRY_GAP_MS + retry_backoff_max_ms(data, ack)),
                             tx_expected_ms(8, 500000, 7, 119, 5000));
    // Defaults (SF8/BW500k/4-7). HIL without backoff: tx_result 20.6-23.7 s after the POST.
    // With the max backoff: about 20.5 + 3 x 1.5 s, roughly 25 s.
    uint32_t e115 = tx_expected_ms(8, 500000, 7, HEADER_LEN + 115, 5000);
    TEST_ASSERT_TRUE(e115 > 23700 && e115 < 27000);
    uint32_t e251 = tx_expected_ms(8, 500000, 7, MAX_FRAME, 5000);
    TEST_ASSERT_TRUE(e251 > e115 && e251 < 28000);
    // Slow config exceeds the old 30 s UI timeout with a single packet.
    TEST_ASSERT_TRUE(tx_expected_ms(12, 125000, 5, MAX_FRAME, 5000) > 30000);
    // Slowest allowed config: several minutes, no uint32 overflow.
    uint32_t slow = tx_expected_ms(12, 7800, 8, MAX_FRAME, 5000);
    TEST_ASSERT_TRUE(slow > 4u * 200000u && slow < 30u * 300000u);
    // Invalid inputs -> 0 (unknown).
    TEST_ASSERT_EQUAL_UINT32(0, tx_expected_ms(13, 125000, 5, 10, 5000));
    TEST_ASSERT_EQUAL_UINT32(0, tx_expected_ms(8, 125000, 7, MAX_FRAME + 1, 5000));
}

// Retry backoff: bounded, covers the window, wider for slower configs.
static void test_retry_backoff() {
    const uint32_t mx = retry_backoff_max_ms(121, 10);
    TEST_ASSERT_EQUAL_UINT32(BACKOFF_BASE_MS + 4u * 131u, mx);
    TEST_ASSERT_EQUAL_UINT32(TX_RETRY_GAP_MS, retry_backoff_ms(0, mx));
    TEST_ASSERT_EQUAL_UINT32(TX_RETRY_GAP_MS + mx - 1, retry_backoff_ms(mx - 1, mx));
    TEST_ASSERT_EQUAL_UINT32(TX_RETRY_GAP_MS, retry_backoff_ms(mx, mx));
    TEST_ASSERT_EQUAL_UINT32(TX_RETRY_GAP_MS, retry_backoff_ms(12345, 0));
    for (uint32_t r = 0; r < 100000; r += 7919) {
        uint32_t b = retry_backoff_ms(r * 2654435761u, mx);
        TEST_ASSERT_TRUE(b >= TX_RETRY_GAP_MS && b < TX_RETRY_GAP_MS + mx);
    }
    // The window must be wider than one data+ACK exchange so retries can separate.
    TEST_ASSERT_TRUE(mx > 2u * (121u + 10u));
    TEST_ASSERT_TRUE(retry_backoff_max_ms(1000, 50) > retry_backoff_max_ms(121, 10));
}

static void test_extract_mac_key() {
    // Valid LORA_MSG payload
    uint32_t k1 = extract_mac_key("{\"name\":\"bob\",\"mac\":\"AABBCCDD\",\"data\":\"{}\"}");
    uint32_t k2 = extract_mac_key("{\"name\":\"alice\",\"mac\":\"11223344\",\"data\":\"{}\"}");
    TEST_ASSERT_NOT_EQUAL(0u, k1);
    TEST_ASSERT_NOT_EQUAL(0u, k2);
    TEST_ASSERT_NOT_EQUAL(k1, k2);
    // Same MAC produces same key
    uint32_t k3 = extract_mac_key("{\"name\":\"charlie\",\"mac\":\"AABBCCDD\",\"data\":\"{}\"}");
    TEST_ASSERT_EQUAL_UINT32(k1, k3);
    // Missing MAC: fallback key (non-zero)
    uint32_t k4 = extract_mac_key("{\"name\":\"bob\",\"data\":\"{}\"}");
    TEST_ASSERT_NOT_EQUAL(0u, k4);
    // Invalid JSON: fallback key
    TEST_ASSERT_NOT_EQUAL(0u, extract_mac_key("garbage"));
    TEST_ASSERT_NOT_EQUAL(0u, extract_mac_key(nullptr));
}

static void test_hash_mac() {
    TEST_ASSERT_NOT_EQUAL(0u, hash_mac("AABBCCDD"));
    TEST_ASSERT_NOT_EQUAL(0u, hash_mac(""));
    TEST_ASSERT_NOT_EQUAL(0u, hash_mac(nullptr));
    TEST_ASSERT_NOT_EQUAL(hash_mac("AA"), hash_mac("BB"));
}

// ---- config ----------------------------------------------------------------------------
static void test_config_valid_and_roundtrip() {
    LoraConfig c;
    TEST_ASSERT_TRUE(config_from_text("{\"freq\":510000000,\"TxPower\":10,\"SpreadingFactor\":8,"
                                      "\"SignalBandwidth\":500000,\"CodingRate4\":7,\"SyncWord\":52}", c));
    TEST_ASSERT_EQUAL_INT32(510000000L, c.freq);
    TEST_ASSERT_EQUAL_INT32(500000L, c.SignalBandwidth);
    LoraConfig c2;
    char js[160];
    TEST_ASSERT_GREATER_THAN(0, config_to_json(c, js, sizeof js));
    TEST_ASSERT_TRUE(config_from_text(js, c2));
    TEST_ASSERT_EQUAL_MEMORY(&c, &c2, sizeof c);
    LoraConfig d = default_config();
    TEST_ASSERT_GREATER_THAN(0, config_to_json(d, js, sizeof js));
    TEST_ASSERT_TRUE(config_from_text(js, c2));
    TEST_ASSERT_EQUAL_INT32(510000000L, c2.freq);
    TEST_ASSERT_EQUAL_INT(52, c2.SyncWord);
    char tiny[8];
    TEST_ASSERT_EQUAL_UINT(0, config_to_json(d, tiny, sizeof tiny));
}

static void test_config_rejects() {
    LoraConfig c;
    const char* err = nullptr;
    TEST_ASSERT_FALSE(config_from_text("not json", c, &err));
    TEST_ASSERT_EQUAL_STRING("json", err);
    TEST_ASSERT_FALSE(config_from_text("{\"freq\":400000000,\"TxPower\":10,\"SpreadingFactor\":8,"
                                       "\"SignalBandwidth\":125000,\"CodingRate4\":7,\"SyncWord\":52}", c, &err));
    TEST_ASSERT_EQUAL_STRING("freq", err);
    TEST_ASSERT_FALSE(config_from_text("{\"freq\":433000000,\"TxPower\":21,\"SpreadingFactor\":8,"
                                       "\"SignalBandwidth\":125000,\"CodingRate4\":7,\"SyncWord\":52}", c, &err));
    TEST_ASSERT_EQUAL_STRING("TxPower", err);
    TEST_ASSERT_FALSE(config_from_text("{\"freq\":433000000,\"TxPower\":10,\"SpreadingFactor\":13,"
                                       "\"SignalBandwidth\":125000,\"CodingRate4\":7,\"SyncWord\":52}", c, &err));
    TEST_ASSERT_EQUAL_STRING("SpreadingFactor", err);
    TEST_ASSERT_FALSE(config_from_text("{\"freq\":433000000,\"TxPower\":10,\"SpreadingFactor\":8,"
                                       "\"SignalBandwidth\":100000,\"CodingRate4\":7,\"SyncWord\":52}", c, &err));
    TEST_ASSERT_EQUAL_STRING("SignalBandwidth", err);
    TEST_ASSERT_FALSE(config_from_text("{\"freq\":433000000,\"TxPower\":10,\"SpreadingFactor\":8,"
                                       "\"SignalBandwidth\":125000,\"CodingRate4\":4,\"SyncWord\":52}", c, &err));
    TEST_ASSERT_EQUAL_STRING("CodingRate4", err);
    TEST_ASSERT_FALSE(config_from_text("{\"freq\":433000000,\"TxPower\":10,\"SpreadingFactor\":8,"
                                       "\"SignalBandwidth\":125000,\"CodingRate4\":5,\"SyncWord\":256}", c, &err));
    TEST_ASSERT_EQUAL_STRING("SyncWord", err);
    TEST_ASSERT_FALSE(config_from_text("{\"freq\":433000000,\"TxPower\":10,\"SpreadingFactor\":8,"
                                       "\"SignalBandwidth\":125000,\"CodingRate4\":5}", c, &err));
    TEST_ASSERT_EQUAL_STRING("SyncWord", err);
    TEST_ASSERT_FALSE(config_from_text("{\"freq\":\"433000000\",\"TxPower\":10,\"SpreadingFactor\":8,"
                                       "\"SignalBandwidth\":125000,\"CodingRate4\":5,\"SyncWord\":1}", c, &err));
    TEST_ASSERT_EQUAL_STRING("freq", err);
    TEST_ASSERT_FALSE(config_from_text("[]", c, &err));
}

// ---- LORA_MSG parsing -------------------------------------------------------------------
static void test_parse_lora_msg() {
    LoraMsgInfo m = parse_lora_msg(
        "{\"name\":\"bob\",\"mac\":\"AA\",\"data\":\"{\\\"pack_type\\\":\\\"msg\\\",\\\"data\\\":\\\"hi\\\"}\"}");
    TEST_ASSERT_EQUAL_INT((int)MsgKind::Text, (int)m.kind);
    TEST_ASSERT_EQUAL_STRING("bob", m.from.c_str());
    TEST_ASSERT_EQUAL_STRING("hi", m.text.c_str());

    m = parse_lora_msg("{\"name\":\"\",\"mac\":\"AA\",\"data\":\"{\\\"pack_type\\\":\\\"action\\\",\\\"data\\\":\\\"sos\\\"}\"}");
    TEST_ASSERT_EQUAL_INT((int)MsgKind::Sos, (int)m.kind);
    TEST_ASSERT_EQUAL_STRING("AA", m.from.c_str());

    // Any other action (e.g. the serial-transfer handshake) is OtherAction.
    m = parse_lora_msg("{\"name\":\"a\",\"mac\":\"AA\",\"data\":\"{\\\"pack_type\\\":\\\"action\\\",\\\"data\\\":"
                       "\\\"file_tx_invite\\\",\\\"tx_id\\\":\\\"ab12\\\"}\"}");
    TEST_ASSERT_EQUAL_INT((int)MsgKind::OtherAction, (int)m.kind);

    TEST_ASSERT_EQUAL_INT((int)MsgKind::Invalid, (int)parse_lora_msg("garbage").kind);
    TEST_ASSERT_EQUAL_INT((int)MsgKind::Invalid, (int)parse_lora_msg("{\"name\":\"a\"}").kind);
    TEST_ASSERT_EQUAL_INT((int)MsgKind::Invalid, (int)parse_lora_msg("{\"name\":\"a\",\"data\":\"{bad\"}").kind);
    TEST_ASSERT_EQUAL_INT((int)MsgKind::Invalid, (int)parse_lora_msg(nullptr).kind);
}


int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_crc8_known_vector);
    RUN_TEST(test_packet_crc_covers_seq);
    RUN_TEST(test_roundtrip_len0);
    RUN_TEST(test_roundtrip_len1);
    RUN_TEST(test_roundtrip_len251);
    RUN_TEST(test_reject_len252);
    RUN_TEST(test_frame_rejects_small_buffer_and_ack_type);
    RUN_TEST(test_serial_frame_roundtrip);
    RUN_TEST(test_bad_crc);
    RUN_TEST(test_len_mismatch_and_short);
    RUN_TEST(test_ack_shape_and_seq_echo);
    RUN_TEST(test_ack_wrong_len_rejected);
    RUN_TEST(test_tx_ack_ok);
    RUN_TEST(test_tx_mismatched_seq_ignored);
    RUN_TEST(test_tx_nack_and_odd_values_retry);
    RUN_TEST(test_tx_retry_exhaustion);
    RUN_TEST(test_tx_mixed_nack_timeout_exhaustion);
    RUN_TEST(test_dedup);
    RUN_TEST(test_dedup_seq_wraps);
    RUN_TEST(test_dedup_uncommitted_not_duplicate);
    RUN_TEST(test_dedup_multi_basic);
    RUN_TEST(test_dedup_multi_new_seq_accepted);
    RUN_TEST(test_dedup_multi_lru_eviction);
    RUN_TEST(test_dedup_multi_peers_independent);
    RUN_TEST(test_dedup_multi_seq_wrap_across_types);
    RUN_TEST(test_dedup_multi_retry_after_interleave);
    RUN_TEST(test_dedup_multi_check_has_no_side_effect);
    RUN_TEST(test_airtime);
    RUN_TEST(test_tx_expected_ms);
    RUN_TEST(test_retry_backoff);
    RUN_TEST(test_extract_mac_key);
    RUN_TEST(test_hash_mac);
    RUN_TEST(test_config_valid_and_roundtrip);
    RUN_TEST(test_config_rejects);
    RUN_TEST(test_parse_lora_msg);
    return UNITY_END();
}
