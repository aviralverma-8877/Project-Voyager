// LoRa network mode on the USB serial port. See net_serial.h for the protocol.
#include "net_serial.h"
#include "support_method.h"
#include <lora/lora_support.h>
#include <ArduinoJson.h>
#include <new>

using namespace lora_proto;

// USB frames up to the adapter MTU (576) are accepted; anything that doesn't fit a
// LoRa frame (NET_MAX_IP) is dropped here (Voyager Link clamps TCP so it fits).
static SlipDecoder<600> s_dec;
static volatile bool s_active = false;
static volatile uint32_t s_last_rx = 0;
static volatile uint32_t s_tx = 0, s_rx = 0, s_drop = 0;

struct NetRxItem { uint16_t len; uint8_t ip[NET_MAX_IP]; };
static QueueHandle_t s_rx_q = NULL;   // LoRa RX task -> USB writer

#define NET_PEERS 8
struct Peer { uint16_t node; int16_t rssi; uint32_t seen; };
static Peer s_peers[NET_PEERS];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void ip_of(uint16_t node, char* out, size_t cap)
{
    snprintf(out, cap, "%u.%u.%u.%u", NET_PREFIX_A, NET_PREFIX_B, node >> 8, node & 0xFF);
}

// Writes LoRa-received packets to USB, one SLIP frame per Serial.write.
static void net_writer_task(void* param)
{
    static uint8_t enc[2 * NET_MAX_IP + 2];
    for (;;) {
        NetRxItem* it = NULL;
        if (xQueueReceive(s_rx_q, &it, portMAX_DELAY) != pdTRUE || it == NULL) continue;
        if (s_active) {
            size_t n = slip_encode(it->ip, it->len, enc, sizeof enc);
            if (n) Serial.write(enc, n);
        }
        delete it;
    }
}

void net_init()
{
    if (s_rx_q) return;
    s_rx_q = xQueueCreate(8, sizeof(NetRxItem*));
    if (s_rx_q == NULL ||
        xTaskCreatePinnedToCore(net_writer_task, "net_writer", 3072, NULL, 1, NULL, 1) != pdPASS)
        serial_print("LoRa network: init failed");
}

bool net_active() { return s_active; }

bool net_enter()
{
    if (lora_serial || s_rx_q == NULL) return false;
    s_dec.reset();
    s_last_rx = millis();
    s_active = true;
    char ip[16];
    ip_of(lora_node_id(), ip, sizeof ip);
    serial_print(String("LoRa network: on, this node is ") + ip);
    show_alert(String("LoRa network on (") + ip + ")");
    return true;
}

void net_leave(const char* why)
{
    if (!s_active) return;
    s_active = false;
    serial_print(String("LoRa network: off (") + why + ")");
}

// Radio queue filling up (or full): tell the PC to slow down, at most every 300 ms.
// The PC paces as if it had the channel alone; with several busy nodes it doesn't.
static void maybe_report_busy(bool dropped)
{
    static uint32_t last = 0;
    int depth = lora_tx_queue_depth();
    if (!dropped && depth < NET_BUSY_DEPTH) return;
    uint32_t now = millis();
    if (now - last < 300) return;
    last = now;
    const uint8_t msg[] = {SLIP_END, 0x00, NET_CTL_BUSY, (uint8_t)(depth > 255 ? 255 : depth), SLIP_END};
    Serial.write(msg, sizeof msg);
}

static void handle_frame(const uint8_t* f, size_t n)
{
    s_last_rx = millis();
    if (f[0] == 0x00) {                          // control
        uint16_t node;
        const uint8_t* ip;
        size_t len;
        if (n >= 2 && f[1] == NET_CTL_LEAVE) {
            net_leave("helper left");
        } else if (n >= 2 && f[1] == NET_CTL_KEEPALIVE) {
            // Tell the PC we're still in network mode: after a reboot we'd be silent,
            // and Voyager Link then repeats the network-start command.
            static const uint8_t ack[] = {SLIP_END, 0x00, NET_CTL_ALIVE_ACK, SLIP_END};
            Serial.write(ack, sizeof ack);
        } else if (net_parse_send_to(f, n, node, ip, len)) {
            // Internet traffic for a gateway node: the IP destination is outside 10.73/16.
            if (len > NET_MAX_IP || node == lora_node_id()) s_drop = s_drop + 1;
            else if (lora_net_send(node, ip, len)) { s_tx = s_tx + 1; maybe_report_busy(false); }
            else { s_drop = s_drop + 1; maybe_report_busy(true); }
        }
        return;                                  // keepalive: s_last_rx already updated
    }
    if (n > NET_MAX_IP || !net_ipv4_ok(f, n)) { s_drop = s_drop + 1; return; }
    uint16_t dst = net_ip_to_node(f + 16);
    if (dst == 0 || dst == lora_node_id()) { s_drop = s_drop + 1; return; }
    if (lora_net_send(dst, f, n)) { s_tx = s_tx + 1; maybe_report_busy(false); }
    else { s_drop = s_drop + 1; maybe_report_busy(true); }   // TX queue full: drop, TCP retransmits
}

void net_feed(uint8_t b)
{
    size_t n = s_dec.feed(b);
    if (n) handle_frame(s_dec.frame(), n);
}

void net_poll()
{
    if (s_active && millis() - s_last_rx > NET_IDLE_TIMEOUT_MS) net_leave("no keepalive from the PC");
}

bool net_deliver(const uint8_t* ip, size_t len)
{
    if (!s_active || s_rx_q == NULL || len > NET_MAX_IP) return false;
    NetRxItem* it = new (std::nothrow) NetRxItem();
    if (it == NULL) return false;
    it->len = (uint16_t)len;
    memcpy(it->ip, ip, len);
    if (xQueueSend(s_rx_q, &it, 0) != pdTRUE) {
        delete it;
        s_drop = s_drop + 1;
        return false;
    }
    s_rx = s_rx + 1;
    return true;
}

void net_heard(uint16_t node, int rssi)
{
    uint32_t now = millis();
    portENTER_CRITICAL(&s_mux);
    int slot = -1, oldest = 0;
    for (int i = 0; i < NET_PEERS; i++) {
        if (s_peers[i].node == node) { slot = i; break; }
        if (s_peers[i].seen < s_peers[oldest].seen) oldest = i;
    }
    if (slot < 0) slot = oldest;
    s_peers[slot].node = node;
    s_peers[slot].rssi = (int16_t)rssi;
    s_peers[slot].seen = now ? now : 1;
    portEXIT_CRITICAL(&s_mux);
}

String net_status_json()
{
    Peer peers[NET_PEERS];
    portENTER_CRITICAL(&s_mux);
    memcpy(peers, s_peers, sizeof peers);
    portEXIT_CRITICAL(&s_mux);

    JsonDocument doc;
    char ip[16];
    ip_of(lora_node_id(), ip, sizeof ip);
    doc["active"] = (bool)s_active;
    doc["node"] = lora_node_id();
    doc["ip"] = ip;
    doc["tx"] = (uint32_t)s_tx;
    doc["rx"] = (uint32_t)s_rx;
    doc["drop"] = (uint32_t)s_drop;
    JsonArray arr = doc["peers"].to<JsonArray>();
    uint32_t now = millis();
    for (int i = 0; i < NET_PEERS; i++) {
        if (peers[i].seen == 0) continue;
        JsonObject o = arr.add<JsonObject>();
        ip_of(peers[i].node, ip, sizeof ip);
        o["ip"] = ip;
        o["rssi"] = peers[i].rssi;
        o["age_s"] = (now - peers[i].seen) / 1000;
    }
    String out;
    serializeJson(doc, out);
    return out;
}
