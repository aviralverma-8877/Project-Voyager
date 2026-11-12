// LoRa radio layer (owner: lora-radio-engineer). Implements Contract 1 (packet v2),
// Contract 5 (lora_config.json) and Contract 6.
#ifndef lora_support
    #define lora_support
    #include <Arduino.h>
    #include <SPI.h>
    #include <LoRa.h>
    #include <core/support_method.h>
    #include "lora_proto.h"
    #define SCK             LSCK
    #define MISO            LMISO
    #define MOSI            LMOSI
    #define SS              LNSS
    #define RST             LRST
    #define IRQ             DIO0
    #define	IMPLEMENTATION	FIFO

    // Contract 1 constants
    #define LORA_MAX_PAYLOAD       251     // max payload bytes per packet (web_server uses it for 413)
    #define LORA_ACK_TIMEOUT_MS    5000
    #define NET_ACK_SLACK_MS       400     // LoRa network unicast: ACK wait = this + 2 ACK airtimes
    #define LORA_MAX_RETRIES       3
    #define LORA_TX_TIMEOUT_MS     10000   // max wait for lora_write_mutex

    // Packet types
    #define LORA_MSG 0               // JSON message / action
    #define RAW_DATA 1               // retired (old chunked file transfer); received ones are dropped
    #define REC_AKNG 2               // acknowledgement
    #define LORA_SERIAL 3            // serial bridge, not ACKed or deduplicated
    #define LORA_NET 4               // LoRa network: [dst][src] + IPv4 (lora_proto T_NET)
    #define LORA_NOACK_FLAG 0x80     // OR-ed into LORA_MSG on the air: no ACK wanted

    static_assert(LORA_MAX_PAYLOAD == lora_proto::MAX_PAYLOAD, "payload limit mismatch");
    static_assert(LORA_MAX_RETRIES == lora_proto::MAX_RETRIES, "retry limit mismatch");
    static_assert(LORA_NOACK_FLAG == lora_proto::T_FLAG_NOACK, "no-ack flag mismatch");

    // send_packets / recv_packets item (Contract 6). Receiver deletes it.
    // id: 0 = device-originated (SOS), no tx_result; otherwise echoed in WS tx_result.
    // no_ack (LORA_MSG only): transmit once, do not wait for an ACK.
    // quiet: a failed transmission is only logged (no alert); used by sensor broadcasts.
    struct QueueParam {
        uint8_t type;
        String message;
        uint32_t id;
        bool no_ack = false;
        bool quiet = false;
    };
    struct DebugQueueParam {
        String message;
    };

    extern SemaphoreHandle_t lora_write_mutex;   // guards every SPI access to the radio

    // Initialize radio, create tasks and attach interrupt. Call once from setup().
    void config_lora();
    // Load config from LittleFS (or defaults) and apply to radio.
    void set_lora_parameters();
    // Validate JSON config, write atomically, apply to radio. Returns false + alert on error.
    bool save_lora_config(String value);

    // Frame and transmit a data packet (blocking). Returns false if payload too large,
    // invalid type, or radio busy. The 2-arg form uses a new seq; 3-arg uses provided seq.
    bool LoRa_send(const String& data, uint8_t type);
    bool LoRa_send(const String& data, uint8_t type, uint8_t seq);
    // Send an ACK/NACK for a received packet (blocking).
    void LoRa_sendAkn(uint8_t seq, uint8_t result);
    // TX task: drains send_packets queue, handles ACK/retry, sends tx_result to WS.
    void LoRa_sendRaw(void* param);
    // LoRa network: this board's node id (from the MAC; IP 10.73.hi.lo), and queueing
    // one IPv4 packet for `dst` (unicast is ACKed/retried, NET_BROADCAST is not).
    // Returns false if the packet is too big or the TX queue is full (dropped, like
    // a router would).
    uint16_t lora_node_id();
    // Time on air (ms) of a frame with `payload_len` bytes under the active config.
    uint32_t lora_frame_airtime_ms(size_t payload_len);
    // Packets waiting in the radio TX queue (all types).
    int lora_tx_queue_depth();
    bool lora_net_send(uint16_t dst, const uint8_t* ip, size_t len);
    // RX task: reads FIFO on interrupt, validates, deduplicates, queues, ACKs.
    void lora_rx_task(void* param);
    // Forwarding task: drains recv_packets queue and sends to WS/SSE/OLED.
    void manage_recv_queue(void* param);
    // Switch radio to continuous receive mode.
    void LoRa_rxMode();
    // Switch radio to idle/transmit mode.
    void LoRa_txMode();
    // DIO0 ISR: only signals lora_rx_task (no SPI, no alloc).
    void onReceive();
    // Send lora_rx message to WebSocket (for chat/actions).
    void send_msg_to_ws(const String& data);
#endif
