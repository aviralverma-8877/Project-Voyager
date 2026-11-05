// Improv Serial (https://www.improv-wifi.com/serial/) framing, header-only and
// Arduino-free so the native tests cover it. Used by improv_serial.cpp so the web
// flasher (ESP Web Tools) can show "Connect to Wi-Fi" right after flashing.
//
// Packet: "IMPROV" | version(1) | type(1) | length(1) | data[length] | checksum(1)
// checksum = sum of every preceding byte (header included) & 0xFF. Senders may
// append '\n' after the checksum; the parser skips anything outside a packet.
#ifndef VOYAGER_IMPROV_H
#define VOYAGER_IMPROV_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace improv {

constexpr uint8_t VERSION = 1;

// Packet types
constexpr uint8_t TYPE_CURRENT_STATE = 0x01;
constexpr uint8_t TYPE_ERROR_STATE = 0x02;
constexpr uint8_t TYPE_RPC = 0x03;
constexpr uint8_t TYPE_RPC_RESULT = 0x04;

// States
constexpr uint8_t STATE_READY = 0x02;  // "authorized": ready to accept credentials
constexpr uint8_t STATE_PROVISIONING = 0x03;
constexpr uint8_t STATE_PROVISIONED = 0x04;

// Errors
constexpr uint8_t ERROR_NONE = 0x00;
constexpr uint8_t ERROR_INVALID_RPC = 0x01;
constexpr uint8_t ERROR_UNKNOWN_RPC = 0x02;
constexpr uint8_t ERROR_UNABLE_TO_CONNECT = 0x03;
constexpr uint8_t ERROR_UNKNOWN = 0xFF;

// RPC commands
constexpr uint8_t CMD_WIFI_SETTINGS = 0x01;
constexpr uint8_t CMD_GET_CURRENT_STATE = 0x02;
constexpr uint8_t CMD_GET_DEVICE_INFO = 0x03;
constexpr uint8_t CMD_GET_WIFI_NETWORKS = 0x04;

constexpr size_t MAX_DATA = 255;
constexpr size_t MAX_PACKET = 6 + 3 + MAX_DATA + 1 + 1;  // + '\n'

struct Packet {
    uint8_t type = 0;
    uint8_t len = 0;
    uint8_t data[MAX_DATA] = {0};
};

// Byte-at-a-time parser. feed() returns:
//   1  a valid packet is in `out`
//  -1  a complete packet with a bad checksum or version (caller answers ERROR_INVALID_RPC)
//   0  need more bytes / byte ignored
class Parser {
public:
    int feed(uint8_t b, Packet& out) {
        static const char HDR[] = "IMPROV";
        if (pos_ < 6) {
            if (b == (uint8_t)HDR[pos_]) {
                pos_++;
            } else {
                pos_ = (b == (uint8_t)'I') ? 1 : 0;  // restart; "IIMPROV" still syncs
            }
            sum_ = 0;
            if (pos_ > 0) for (size_t i = 0; i < pos_; i++) sum_ += (uint8_t)HDR[i];
            return 0;
        }
        if (pos_ == 6) { version_ = b; sum_ += b; pos_++; return 0; }
        if (pos_ == 7) { pkt_.type = b; sum_ += b; pos_++; return 0; }
        if (pos_ == 8) { pkt_.len = b; sum_ += b; pos_++; got_ = 0; return 0; }
        if (got_ < pkt_.len) {
            pkt_.data[got_++] = b;
            sum_ += b;
            return 0;
        }
        // checksum byte
        pos_ = 0;
        if (b != (uint8_t)(sum_ & 0xFF) || version_ != VERSION) return -1;
        out = pkt_;
        return 1;
    }
    void reset() { pos_ = 0; }

private:
    size_t pos_ = 0;
    size_t got_ = 0;
    uint8_t version_ = 0;
    uint32_t sum_ = 0;
    Packet pkt_;
};

// Builds one packet into out (cap >= 6+3+len+2). Returns its size, or 0 if it doesn't fit.
inline size_t build(uint8_t type, const uint8_t* data, size_t len, uint8_t* out, size_t cap) {
    if (len > MAX_DATA || cap < 6 + 3 + len + 2) return 0;
    memcpy(out, "IMPROV", 6);
    out[6] = VERSION;
    out[7] = type;
    out[8] = (uint8_t)len;
    if (len) memcpy(out + 9, data, len);
    uint32_t sum = 0;
    for (size_t i = 0; i < 9 + len; i++) sum += out[i];
    out[9 + len] = (uint8_t)(sum & 0xFF);
    out[10 + len] = '\n';
    return 11 + len;
}

inline size_t build_state(uint8_t state, uint8_t* out, size_t cap) {
    return build(TYPE_CURRENT_STATE, &state, 1, out, cap);
}

inline size_t build_error(uint8_t err, uint8_t* out, size_t cap) {
    return build(TYPE_ERROR_STATE, &err, 1, out, cap);
}

// RPC result: command | total length | (len | string)*. Strings longer than 255 are cut.
inline size_t build_rpc_result(uint8_t cmd, const char* const* strs, size_t n, uint8_t* out, size_t cap) {
    uint8_t data[MAX_DATA];
    size_t d = 2;
    for (size_t i = 0; i < n; i++) {
        size_t sl = strs[i] ? strlen(strs[i]) : 0;
        if (sl > 255) sl = 255;
        if (d + 1 + sl > MAX_DATA) return 0;
        data[d++] = (uint8_t)sl;
        if (sl) memcpy(data + d, strs[i], sl);
        d += sl;
    }
    data[0] = cmd;
    data[1] = (uint8_t)(d - 2);
    return build(TYPE_RPC_RESULT, data, d, out, cap);
}

// RPC packet data: command | length | payload. Returns false if malformed.
inline bool rpc_command(const Packet& p, uint8_t& cmd, const uint8_t*& payload, size_t& plen) {
    if (p.type != TYPE_RPC || p.len < 2) return false;
    if ((size_t)p.data[1] + 2 > p.len) return false;
    cmd = p.data[0];
    payload = p.data + 2;
    plen = p.data[1];
    return true;
}

// WIFI_SETTINGS payload: ssid_len | ssid | pass_len | pass. ssid must be 1-32 bytes,
// pass 0-64. Outputs are NUL-terminated.
inline bool parse_wifi_settings(const uint8_t* payload, size_t plen, char ssid[33], char pass[65]) {
    if (plen < 2) return false;
    size_t sl = payload[0];
    if (sl < 1 || sl > 32 || 1 + sl + 1 > plen) return false;
    size_t pl = payload[1 + sl];
    if (pl > 64 || 1 + sl + 1 + pl > plen) return false;
    memcpy(ssid, payload + 1, sl);
    ssid[sl] = '\0';
    memcpy(pass, payload + 2 + sl, pl);
    pass[pl] = '\0';
    return true;
}

}  // namespace improv

#endif
