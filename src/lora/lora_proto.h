// Pure (Arduino-free) helpers for the LoRa v2 protocol (Contract 1) and the
// LoRa config range checks (Contract 5). Header-only so the native Unity tests
// (test/test_lora) and the firmware (lora_support.cpp) share the same code.
// Depends only on the C/C++ standard library and ArduinoJson.
#ifndef LORA_PROTO_H
#define LORA_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <ArduinoJson.h>

namespace lora_proto {

// ---- Contract 1 constants -------------------------------------------------
constexpr uint8_t  T_LORA_MSG    = 0;
constexpr uint8_t  T_RAW_DATA    = 1;
constexpr uint8_t  T_REC_AKNG    = 2;
constexpr uint8_t  T_LORA_SERIAL = 3;
constexpr uint8_t  T_NET         = 4;     // LoRa network: [dst u16][src u16] + IPv4 packet
constexpr size_t   HEADER_LEN    = 4;     // [len][type][seq][crc8]
constexpr size_t   MAX_PAYLOAD   = 251;   // 255 FIFO bytes - header
constexpr size_t   MAX_FRAME     = HEADER_LEN + MAX_PAYLOAD;
constexpr uint8_t  MAX_RETRIES   = 3;     // retransmissions after the first attempt
constexpr uint8_t  ACK_OK        = 1;
constexpr uint8_t  ACK_RETRY     = 0;

// ---- CRC-8 (poly 0x07, init 0x00, no reflection, no final xor) -------------
inline uint8_t crc8_update(uint8_t crc, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}
inline uint8_t crc8(const uint8_t* data, size_t len) { return crc8_update(0, data, len); }

// CRC over the seq byte followed by the payload.
inline uint8_t packet_crc(uint8_t seq, const uint8_t* payload, size_t len) {
    uint8_t c = crc8_update(0, &seq, 1);
    return crc8_update(c, payload, len);
}

// ---- framing ----------------------------------------------------------------
// Returns the frame length, or 0 if the payload is too long, the type is not a
// data type, or `out` is too small. Use frame_ack() for REC_AKNG.
inline size_t frame_packet(uint8_t type, uint8_t seq, const uint8_t* payload, size_t len,
                           uint8_t* out, size_t out_cap) {
    if (len > MAX_PAYLOAD || type == T_REC_AKNG || type > T_NET) return 0;
    if (out == nullptr || out_cap < HEADER_LEN + len) return 0;
    if (len > 0 && payload == nullptr) return 0;
    out[0] = (uint8_t)len;
    out[1] = type;
    out[2] = seq;
    out[3] = packet_crc(seq, payload, len);
    if (len) memcpy(out + HEADER_LEN, payload, len);
    return HEADER_LEN + len;
}

// REC_AKNG: len=1, seq echoes the ACKed packet, crc=0, payload = result byte.
inline size_t frame_ack(uint8_t seq, uint8_t result, uint8_t* out, size_t out_cap) {
    if (out == nullptr || out_cap < HEADER_LEN + 1) return 0;
    out[0] = 1;
    out[1] = T_REC_AKNG;
    out[2] = seq;
    out[3] = 0;
    out[4] = result;
    return HEADER_LEN + 1;
}

enum class ParseErr : uint8_t { None, TooShort, LenMismatch, BadType, BadCrc, BadAck };

struct Parsed {
    bool ok;
    ParseErr err;
    uint8_t type;
    uint8_t seq;
    uint8_t len;
    const uint8_t* payload;   // points into the input buffer
};

inline Parsed parse_packet(const uint8_t* buf, size_t n) {
    Parsed p{false, ParseErr::None, 0, 0, 0, nullptr};
    if (buf == nullptr || n < HEADER_LEN) { p.err = ParseErr::TooShort; return p; }
    p.len = buf[0];
    p.type = buf[1];
    p.seq = buf[2];
    p.payload = buf + HEADER_LEN;
    if (n != HEADER_LEN + (size_t)p.len) { p.err = ParseErr::LenMismatch; return p; }
    if (p.type > T_NET) { p.err = ParseErr::BadType; return p; }
    if (p.type == T_REC_AKNG) {
        if (p.len != 1) { p.err = ParseErr::BadAck; return p; }
        p.ok = true;
        return p;
    }
    if (buf[3] != packet_crc(p.seq, p.payload, p.len)) { p.err = ParseErr::BadCrc; return p; }
    p.ok = true;
    return p;
}

// ---- sender retry / ACK state machine ----------------------------------------
enum class TxAction : uint8_t {
    Wait,        // ignore this event, keep waiting for the ACK of the current attempt
    Retransmit,  // send the same frame (same seq) again
    Success,     // ACK 1 with matching seq
    Fail         // retries exhausted
};

struct TxRetry {
    uint8_t seq = 0;
    uint8_t attempts = 0;     // transmissions made so far
    bool done = false;
};

inline void tx_begin(TxRetry& t, uint8_t seq) { t.seq = seq; t.attempts = 1; t.done = false; }

inline TxAction tx_next_attempt_or_fail(TxRetry& t) {
    if (t.attempts > MAX_RETRIES) { t.done = true; return TxAction::Fail; }
    t.attempts++;
    return TxAction::Retransmit;
}

// An ACK arrived. Mismatched seq is ignored; any value other than 1 is a NACK.
inline TxAction tx_on_ack(TxRetry& t, uint8_t ack_seq, uint8_t value) {
    if (t.done) return TxAction::Wait;
    if (ack_seq != t.seq) return TxAction::Wait;
    if (value == ACK_OK) { t.done = true; return TxAction::Success; }
    return tx_next_attempt_or_fail(t);
}

inline TxAction tx_on_timeout(TxRetry& t) {
    if (t.done) return TxAction::Wait;
    return tx_next_attempt_or_fail(t);
}

// ---- receiver seq dedup ---------------------------------------------------------
// Simple single-source dedup (legacy, kept for test compatibility).
struct RxDedup {
    bool have = false;
    uint8_t last_seq = 0;
};

// True if this data packet repeats the last *accepted* one: ACK 1, don't deliver.
inline bool rx_is_duplicate(const RxDedup& d, uint8_t seq) { return d.have && d.last_seq == seq; }
// Call after the packet was actually delivered (queued).
inline void rx_commit(RxDedup& d, uint8_t seq) { d.have = true; d.last_seq = seq; }

// ---- per-source dedup (multi-peer) -----------------------------------------------
// Tracks the last accepted seq for up to N sources, identified by a 32-bit key
// (MAC hash). Uses LRU replacement when full. Key 0 is unused (hash_mac never returns it).
//
// A sender uses ONE seq counter for all its packets (including unACKed LORA_SERIAL
// frames, which are not tracked here). Without aging, a LORA_MSG that comes exactly
// 256*k packets after the previous one would match the stale entry and be dropped as
// a "duplicate".
// A real duplicate is a retry of the packet we just ACKed, so it arrives within a few
// accepted packets. An entry only counts if it was accepted within the last
// DEDUP_WINDOW accepted packets.
constexpr size_t DEDUP_SLOTS = 4;
constexpr uint32_t DEDUP_WINDOW = 16;  // must be well below 256

struct RxDedupEntry {
    uint32_t key = 0;
    uint32_t at = 0;      // value of RxDedupMulti::accepted when committed
    uint8_t seq = 0;
    bool used = false;
};

struct RxDedupMulti {
    RxDedupEntry entries[DEDUP_SLOTS] = {};
    size_t lru = 0;          // next slot to replace when all are used
    uint32_t accepted = 0;   // total commits so far
};

// Simple hash for MAC string (e.g., "AABBCCDD"). Returns non-zero for valid input.
inline uint32_t hash_mac(const char* mac) {
    if (mac == nullptr || mac[0] == '\0') return 1;  // fallback, never 0
    uint32_t h = 5381;
    for (const char* p = mac; *p; ++p)
        h = ((h << 5) + h) ^ (uint8_t)*p;
    return h ? h : 1;  // never return 0
}

// Extract MAC from LORA_MSG payload JSON. Returns hash, or 1 if parsing fails.
inline uint32_t extract_mac_key(const char* payload) {
    if (payload == nullptr) return 1;
    JsonDocument doc;
    if (deserializeJson(doc, payload)) return 1;
    const char* mac = doc["mac"] | (const char*)nullptr;
    return hash_mac(mac);
}

// Existing entry for `key`, or nullptr. No side effects.
inline const RxDedupEntry* rx_dedup_lookup(const RxDedupMulti& d, uint32_t key) {
    for (size_t i = 0; i < DEDUP_SLOTS; ++i)
        if (d.entries[i].used && d.entries[i].key == key)
            return &d.entries[i];
    return nullptr;
}

// Find or allocate a slot for the given key (evicts when full). Used by commit only.
inline RxDedupEntry* rx_dedup_find(RxDedupMulti& d, uint32_t key) {
    for (size_t i = 0; i < DEDUP_SLOTS; ++i)
        if (d.entries[i].used && d.entries[i].key == key)
            return &d.entries[i];
    for (size_t i = 0; i < DEDUP_SLOTS; ++i)
        if (!d.entries[i].used)
            return &d.entries[i];
    RxDedupEntry* e = &d.entries[d.lru];
    d.lru = (d.lru + 1) % DEDUP_SLOTS;
    e->used = false;
    return e;
}

// True if this repeats the recently accepted packet from the same source.
// Doesn't modify `d`; a check that's never committed evicts nothing.
inline bool rx_is_duplicate_multi(const RxDedupMulti& d, uint32_t key, uint8_t seq) {
    const RxDedupEntry* e = rx_dedup_lookup(d, key);
    return e != nullptr && e->seq == seq && (uint32_t)(d.accepted - e->at) < DEDUP_WINDOW;
}

inline void rx_commit_multi(RxDedupMulti& d, uint32_t key, uint8_t seq) {
    RxDedupEntry* e = rx_dedup_find(d, key);
    e->key = key;
    e->seq = seq;
    e->at = d.accepted;
    e->used = true;
    d.accepted++;
}

// ---- time on air (Semtech SX127x formula) ------------------------------------------
// Explicit header, 8-symbol preamble, payload CRC counted (an upper bound whether or
// not the radio has CRC on), low-data-rate optimisation when a symbol lasts > 16 ms
// (the LoRa library uses the same rule). `cr4` is 5..8. Returns ms, rounded up.
inline uint32_t airtime_ms(int sf, long bw_hz, int cr4, size_t payload_len) {
    if (sf < 6 || sf > 12 || bw_hz <= 0 || cr4 < 5 || cr4 > 8) return 0;
    const double tsym_ms = (double)(1L << sf) * 1000.0 / (double)bw_hz;
    const int de = tsym_ms > 16.0 ? 1 : 0;
    const long num = 8L * (long)payload_len - 4L * sf + 28 + 16;   // CRC on, explicit header
    const long den = 4L * (sf - 2 * de);
    long n = num > 0 ? (num + den - 1) / den : 0;
    const double symbols = (8 + 4.25) + 8 + (double)(n * cr4);
    const double ms = symbols * tsym_ms;
    return (uint32_t)ms + 1;
}

// ---- Contract 3 tx_started: worst-case time until tx_result -----------------------
constexpr uint8_t  TX_ATTEMPTS        = MAX_RETRIES + 1;   // first send + retries
constexpr uint32_t TX_RETRY_GAP_MS    = 10;                // delay between attempts
constexpr size_t   ACK_FRAME_LEN      = HEADER_LEN + 1;

// Random backoff before each retransmission. Without it, two boards that start sending
// at the same moment stay in lockstep (same TX time, same ACK timeout), so every retry
// collides and neither ever hears the other (seen in two-board HIL). The window is
// several data+ACK exchanges wide, so a retry usually lands in a free slot.
constexpr uint32_t BACKOFF_BASE_MS = 1000;
inline uint32_t retry_backoff_max_ms(uint32_t data_airtime_ms, uint32_t ack_airtime_ms) {
    return BACKOFF_BASE_MS + 4u * (data_airtime_ms + ack_airtime_ms);
}
// `rnd` is any 32-bit random value. Result is in [TX_RETRY_GAP_MS, TX_RETRY_GAP_MS + max).
inline uint32_t retry_backoff_ms(uint32_t rnd, uint32_t max_ms) {
    return TX_RETRY_GAP_MS + (max_ms ? rnd % max_ms : 0);
}

// Every attempt times out and every backoff is the longest:
// TX_ATTEMPTS x (airtime(frame) + ack_timeout_ms + airtime(ACK))
//   + MAX_RETRIES x (TX_RETRY_GAP_MS + retry_backoff_max_ms).
// This matches the sender: each attempt waits ack_timeout_ms + airtime(ACK) after
// TX_DONE. Mutex waits aren't included; the UI adds its own slack. Returns 0 for an
// invalid config or a frame longer than a LoRa packet.
inline uint32_t tx_expected_ms(int sf, long bw_hz, int cr4, size_t frame_len, uint32_t ack_timeout_ms) {
    if (frame_len > MAX_FRAME) return 0;
    const uint32_t data = airtime_ms(sf, bw_hz, cr4, frame_len);
    const uint32_t ack  = airtime_ms(sf, bw_hz, cr4, ACK_FRAME_LEN);
    if (data == 0 || ack == 0) return 0;
    return (uint32_t)TX_ATTEMPTS * (data + ack_timeout_ms + ack) +
           (uint32_t)MAX_RETRIES * (TX_RETRY_GAP_MS + retry_backoff_max_ms(data, ack));
}

// ---- Contract 5: LoRa radio config --------------------------------------------
struct LoraConfig {
    long freq;            // Hz
    int TxPower;
    int SpreadingFactor;
    long SignalBandwidth; // Hz
    int CodingRate4;
    int SyncWord;
};

// Compiled defaults (Contract 5: 510 MHz, SyncWord 52; rest matches lora_default.json).
inline LoraConfig default_config() { return LoraConfig{510000000L, 10, 8, 500000L, 7, 52}; }

inline bool valid_bandwidth(long bw) {
    static const long kBw[] = {7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000};
    for (long v : kBw) if (v == bw) return true;
    return false;
}

// Fills `out` and returns true only if every key is present, numeric and in range.
// `err` (if given) receives the name of the first offending key.
inline bool config_from_json(JsonVariantConst v, LoraConfig& out, const char** err = nullptr) {
    auto fail = [&](const char* k) { if (err) *err = k; return false; };
    auto num = [&](const char* k, long lo, long hi, long& dst) -> bool {
        JsonVariantConst f = v[k];
        if (f.isNull() || !(f.is<long>() || f.is<double>())) return false;
        double d = f.as<double>();
        if (d != (double)(long)d) return false;          // integers only
        long x = (long)d;
        if (x < lo || x > hi) return false;
        dst = x;
        return true;
    };
    if (!v.is<JsonObjectConst>()) return fail("object");
    long freq, txp, sf, bw, cr, sw;
    if (!num("freq", 410000000L, 525000000L, freq)) return fail("freq");
    if (!num("TxPower", 2, 20, txp)) return fail("TxPower");
    if (!num("SpreadingFactor", 6, 12, sf)) return fail("SpreadingFactor");
    if (!num("SignalBandwidth", 7800, 500000, bw) || !valid_bandwidth(bw)) return fail("SignalBandwidth");
    if (!num("CodingRate4", 5, 8, cr)) return fail("CodingRate4");
    if (!num("SyncWord", 0, 255, sw)) return fail("SyncWord");
    out = LoraConfig{freq, (int)txp, (int)sf, bw, (int)cr, (int)sw};
    return true;
}

inline bool config_from_text(const char* text, LoraConfig& out, const char** err = nullptr) {
    if (text == nullptr) { if (err) *err = "json"; return false; }
    JsonDocument doc;
    if (deserializeJson(doc, text)) { if (err) *err = "json"; return false; }
    return config_from_json(doc.as<JsonVariantConst>(), out, err);
}

// Serializes to `out` (NUL-terminated). Returns the length, or 0 if `out` is too small.
inline size_t config_to_json(const LoraConfig& c, char* out, size_t cap) {
    JsonDocument doc;
    doc["freq"] = c.freq;
    doc["TxPower"] = c.TxPower;
    doc["SpreadingFactor"] = c.SpreadingFactor;
    doc["SignalBandwidth"] = c.SignalBandwidth;
    doc["CodingRate4"] = c.CodingRate4;
    doc["SyncWord"] = c.SyncWord;
    if (out == nullptr || cap == 0 || measureJson(doc) + 1 > cap) return 0;
    return serializeJson(doc, out, cap);
}

// ---- LORA_MSG payload (Contract 1) --------------------------------------------
enum class MsgKind : uint8_t { Invalid, Text, Sos, OtherAction };

struct LoraMsgInfo {
    MsgKind kind = MsgKind::Invalid;
    std::string from;          // name, or mac if name is empty
    std::string text;          // pack_type msg
};

inline LoraMsgInfo parse_lora_msg(const char* payload) {
    LoraMsgInfo info;
    if (payload == nullptr) return info;
    JsonDocument outer;
    if (deserializeJson(outer, payload)) return info;
    const char* name = outer["name"] | "";
    const char* mac = outer["mac"] | "";
    info.from = (name[0] != '\0') ? name : mac;
    const char* inner_txt = outer["data"] | (const char*)nullptr;
    if (inner_txt == nullptr) return info;
    JsonDocument inner;
    if (deserializeJson(inner, inner_txt)) return info;
    const char* pt = inner["pack_type"] | "";
    if (strcmp(pt, "msg") == 0) {
        const char* t = inner["data"] | (const char*)nullptr;
        if (t == nullptr) return info;
        info.text = t;
        info.kind = MsgKind::Text;
    } else if (strcmp(pt, "action") == 0) {
        const char* a = inner["data"] | "";
        if (strcmp(a, "sos") == 0) info.kind = MsgKind::Sos;
        else info.kind = MsgKind::OtherAction;
    }
    return info;
}


// ---- LoRa network (T_NET): IPv4 over the radio --------------------------------
// Every board is a node on one shared radio channel. A node id is 16 bits taken
// from the MAC, and node X.Y owns IPv4 10.73.X.Y/16, so no configuration or ARP
// is needed: the IP destination names the node. Payload of a T_NET frame:
//   [dst hi][dst lo][src hi][src lo] + one whole IPv4 packet (<= NET_MAX_IP bytes).
// Unicast frames are ACKed/retried like LORA_MSG; dst NET_BROADCAST is not ACKed.
constexpr uint8_t  NET_PREFIX_A  = 10;
constexpr uint8_t  NET_PREFIX_B  = 73;
constexpr size_t   NET_HDR       = 4;
constexpr size_t   NET_MAX_IP    = MAX_PAYLOAD - NET_HDR;   // 247
constexpr uint16_t NET_BROADCAST = 0xFFFF;

// Node id from the last two MAC bytes. Each byte is kept in 1..254 so the address
// 10.73.X.Y never ends in .0 or .255 in either octet.
inline uint8_t net_fix_octet(uint8_t b) { return b == 0 ? 1 : (b == 255 ? 254 : b); }
inline uint16_t net_node_id(const uint8_t mac[6]) {
    return (uint16_t)((net_fix_octet(mac[4]) << 8) | net_fix_octet(mac[5]));
}

// IPv4 destination -> node: 0 = not ours (drop), NET_BROADCAST for broadcast.
inline uint16_t net_ip_to_node(const uint8_t ip[4]) {
    if (ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255) return NET_BROADCAST;
    if (ip[0] != NET_PREFIX_A || ip[1] != NET_PREFIX_B) return 0;
    if (ip[2] == 255 && ip[3] == 255) return NET_BROADCAST;               // 10.73.255.255
    if (ip[2] == 0 || ip[2] == 255 || ip[3] == 0 || ip[3] == 255) return 0;
    return (uint16_t)((ip[2] << 8) | ip[3]);
}

// Minimal IPv4 check: version 4, header length sane, total length == len.
inline bool net_ipv4_ok(const uint8_t* ip, size_t len) {
    if (ip == nullptr || len < 20 || (ip[0] >> 4) != 4) return false;
    size_t ihl = (size_t)(ip[0] & 0x0F) * 4;
    size_t total = ((size_t)ip[2] << 8) | ip[3];
    return ihl >= 20 && ihl <= total && total == len;
}

// Builds a T_NET payload. Returns its length, 0 if the packet doesn't fit.
inline size_t net_build(uint16_t dst, uint16_t src, const uint8_t* ip, size_t len,
                        uint8_t* out, size_t cap) {
    if (len == 0 || len > NET_MAX_IP || cap < NET_HDR + len || ip == nullptr) return 0;
    out[0] = (uint8_t)(dst >> 8); out[1] = (uint8_t)dst;
    out[2] = (uint8_t)(src >> 8); out[3] = (uint8_t)src;
    memcpy(out + NET_HDR, ip, len);
    return NET_HDR + len;
}

struct NetFrame { uint16_t dst; uint16_t src; const uint8_t* ip; size_t len; };

inline bool net_parse(const uint8_t* payload, size_t n, NetFrame& f) {
    if (payload == nullptr || n <= NET_HDR) return false;
    f.dst = (uint16_t)((payload[0] << 8) | payload[1]);
    f.src = (uint16_t)((payload[2] << 8) | payload[3]);
    f.ip = payload + NET_HDR;
    f.len = n - NET_HDR;
    return f.src != 0 && f.src != NET_BROADCAST && f.dst != 0 && net_ipv4_ok(f.ip, f.len);
}

// Dedup key for NET unicast from one node: a fixed tag in the high half keeps it
// apart from the LORA_MSG keys (32-bit MAC hashes) in practice.
inline uint32_t net_dedup_key(uint16_t src) { return 0x4E450000u | src; }

// ---- USB control frames in network mode (first byte 0x00, never IPv4) ----------
constexpr uint8_t NET_CTL_KEEPALIVE = 0x01;   // 00 01
constexpr uint8_t NET_CTL_LEAVE     = 0x02;   // 00 02
constexpr uint8_t NET_CTL_SEND_TO   = 0x03;   // 00 03 [node hi][node lo] + IPv4 packet
constexpr uint8_t NET_CTL_ALIVE_ACK = 0x81;   // board -> PC: answer to 00 01 (still in network mode)
constexpr uint8_t NET_CTL_BUSY      = 0x82;   // board -> PC: 00 82 [queued]: radio queue filling, slow down
constexpr uint8_t NET_BUSY_DEPTH    = 6;      // TX queue depth that triggers 00 82

// "Send this IPv4 packet to node X" whatever its IP destination: how a PC reaches
// the internet through a gateway node (the packet's destination is outside 10.73/16).
// Returns false if malformed, the node is invalid or the packet isn't IPv4.
inline bool net_parse_send_to(const uint8_t* f, size_t n, uint16_t& node, const uint8_t*& ip, size_t& len) {
    if (f == nullptr || n < 4 || f[0] != 0x00 || f[1] != NET_CTL_SEND_TO) return false;
    node = (uint16_t)((f[2] << 8) | f[3]);
    ip = f + 4;
    len = n - 4;
    if (node == 0 || node == NET_BROADCAST) return false;
    return net_ipv4_ok(ip, len);
}

// ---- SLIP (RFC 1055): IP packets over the USB serial link in network mode ----
constexpr uint8_t SLIP_END = 0xC0, SLIP_ESC = 0xDB, SLIP_ESC_END = 0xDC, SLIP_ESC_ESC = 0xDD;

// Encodes one frame (END + escaped bytes + END). Returns its size, 0 if cap is too
// small (worst case 2 * len + 2).
inline size_t slip_encode(const uint8_t* in, size_t len, uint8_t* out, size_t cap) {
    if (cap < 2) return 0;
    size_t o = 0;
    out[o++] = SLIP_END;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = in[i];
        bool esc = (b == SLIP_END || b == SLIP_ESC);
        if (o + (esc ? 2 : 1) > cap - 1) return 0;
        if (esc) {
            out[o++] = SLIP_ESC;
            out[o++] = (b == SLIP_END) ? SLIP_ESC_END : SLIP_ESC_ESC;
        } else {
            out[o++] = b;
        }
    }
    out[o++] = SLIP_END;
    return o;
}

// Byte-at-a-time decoder. feed() returns the frame length when a non-empty frame
// ends (bytes in frame()), else 0. Oversized or badly escaped frames are dropped.
template <size_t CAP>
class SlipDecoder {
public:
    size_t feed(uint8_t b) {
        if (b == SLIP_END) {
            size_t n = (bad_ || esc_) ? 0 : len_;
            len_ = 0; esc_ = false; bad_ = false;
            return n;
        }
        if (bad_) return 0;
        if (esc_) {
            esc_ = false;
            if (b == SLIP_ESC_END) b = SLIP_END;
            else if (b == SLIP_ESC_ESC) b = SLIP_ESC;
            else { bad_ = true; return 0; }
        } else if (b == SLIP_ESC) {
            esc_ = true;
            return 0;
        }
        if (len_ >= CAP) { bad_ = true; return 0; }
        buf_[len_++] = b;
        return 0;
    }
    const uint8_t* frame() const { return buf_; }
    void reset() { len_ = 0; esc_ = false; bad_ = false; }

private:
    uint8_t buf_[CAP];
    size_t len_ = 0;
    bool esc_ = false;
    bool bad_ = false;
};

}  // namespace lora_proto

#endif
