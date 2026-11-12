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
// Flag bit in the type byte: "do not acknowledge". Only valid on LORA_MSG.
// The sender transmits once and the receiver delivers without sending REC_AKNG.
constexpr uint8_t  T_FLAG_NOACK  = 0x80;
constexpr uint8_t  T_TYPE_MASK   = 0x7F;
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
    const uint8_t base = type & T_TYPE_MASK;
    if (len > MAX_PAYLOAD || base == T_REC_AKNG || base > T_LORA_SERIAL) return 0;
    if ((type & T_FLAG_NOACK) && base != T_LORA_MSG) return 0;
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
    bool noack;               // T_FLAG_NOACK was set: deliver, never ACK / NACK
};

inline Parsed parse_packet(const uint8_t* buf, size_t n) {
    Parsed p{false, ParseErr::None, 0, 0, 0, nullptr, false};
    if (buf == nullptr || n < HEADER_LEN) { p.err = ParseErr::TooShort; return p; }
    p.len = buf[0];
    p.type = buf[1] & T_TYPE_MASK;   // base type; the flag is reported in p.noack
    p.noack = (buf[1] & T_FLAG_NOACK) != 0;
    p.seq = buf[2];
    p.payload = buf + HEADER_LEN;
    if (n != HEADER_LEN + (size_t)p.len) { p.err = ParseErr::LenMismatch; return p; }
    if (p.type > T_LORA_SERIAL || (p.noack && p.type != T_LORA_MSG)) { p.err = ParseErr::BadType; return p; }
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
// (MAC hash). Uses LRU replacement when full. Key 0 is reserved for RAW_DATA.
constexpr size_t DEDUP_SLOTS = 4;
constexpr uint32_t DEDUP_KEY_RAW = 0;  // RAW_DATA packets share this slot

struct RxDedupEntry {
    uint32_t key = 0;
    uint8_t seq = 0;
    bool used = false;
};

struct RxDedupMulti {
    RxDedupEntry entries[DEDUP_SLOTS] = {};
    size_t lru = 0;  // next slot to replace when all are used
};

// Simple hash for MAC string (e.g., "AABBCCDD"). Returns non-zero for valid input.
inline uint32_t hash_mac(const char* mac) {
    if (mac == nullptr || mac[0] == '\0') return 1;  // fallback, distinct from DEDUP_KEY_RAW
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

// Find or allocate a slot for the given key.
inline RxDedupEntry* rx_dedup_find(RxDedupMulti& d, uint32_t key) {
    // First pass: exact match
    for (size_t i = 0; i < DEDUP_SLOTS; ++i)
        if (d.entries[i].used && d.entries[i].key == key)
            return &d.entries[i];
    // Second pass: unused slot
    for (size_t i = 0; i < DEDUP_SLOTS; ++i)
        if (!d.entries[i].used)
            return &d.entries[i];
    // All used: LRU replacement
    RxDedupEntry* e = &d.entries[d.lru];
    d.lru = (d.lru + 1) % DEDUP_SLOTS;
    e->used = false;
    return e;
}

inline bool rx_is_duplicate_multi(RxDedupMulti& d, uint32_t key, uint8_t seq) {
    RxDedupEntry* e = rx_dedup_find(d, key);
    return e->used && e->key == key && e->seq == seq;
}

inline void rx_commit_multi(RxDedupMulti& d, uint32_t key, uint8_t seq) {
    RxDedupEntry* e = rx_dedup_find(d, key);
    e->key = key;
    e->seq = seq;
    e->used = true;
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
enum class MsgKind : uint8_t { Invalid, Text, Sos, FileStart, FileEnd, OtherAction, Sensor };

struct LoraMsgInfo {
    MsgKind kind = MsgKind::Invalid;
    std::string from;          // name, or mac if name is empty
    std::string text;          // pack_type msg
    std::string file_name;     // enable_file_tx_mode
    long total_packets = 0;    // enable/disable_file_tx_mode
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
        info.total_packets = inner["total_packets"] | 0L;
        if (strcmp(a, "sos") == 0) info.kind = MsgKind::Sos;
        else if (strcmp(a, "enable_file_tx_mode") == 0) {
            info.kind = MsgKind::FileStart;
            info.file_name = inner["name"] | "";
        } else if (strcmp(a, "disable_file_tx_mode") == 0) info.kind = MsgKind::FileEnd;
        else info.kind = MsgKind::OtherAction;
    } else if (strcmp(pt, "sensor") == 0) {
        info.kind = MsgKind::Sensor;   // readings are shown by the web UI only
    }
    return info;
}

// RX file progress as 0-100 (capped), for oled_progress.
inline uint8_t progress_pct(long received, long total) {
    if (total <= 0 || received <= 0) return 0;
    if (received >= total) return 100;
    return (uint8_t)((received * 100L) / total);
}

}  // namespace lora_proto

#endif
