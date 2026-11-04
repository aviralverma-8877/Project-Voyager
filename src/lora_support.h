// LoRa radio layer (owner: lora-radio-engineer). Implements Contract 1 (packet v2),
// Contract 4 (RAW_DATA producer), Contract 5 (lora_config.json) and Contract 6.
#ifndef lora_support
    #define lora_support
    #include <Arduino.h>
    #include <SPI.h>
    #include <LoRa.h>
    #include <support_method.h>
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
    #define LORA_MAX_RETRIES       3
    #define LORA_TX_TIMEOUT_MS     10000   // max wait for lora_write_mutex

    // Packet types
    #define LORA_MSG 0               // JSON message / action
    #define RAW_DATA 1               // file chunk (slice of a data-URL)
    #define REC_AKNG 2               // acknowledgement
    #define LORA_SERIAL 3            // serial bridge, not ACKed or deduplicated

    static_assert(LORA_MAX_PAYLOAD == lora_proto::MAX_PAYLOAD, "payload limit mismatch");
    static_assert(LORA_MAX_RETRIES == lora_proto::MAX_RETRIES, "retry limit mismatch");

    // send_packets / recv_packets item (Contract 6). Receiver deletes it.
    // id: 0 = device-originated (SOS), no tx_result; otherwise echoed in WS tx_result.
    struct QueueParam {
        uint8_t type;
        String message;
        uint32_t id;
    };
    struct DebugQueueParam {
        String message;
    };

    extern SemaphoreHandle_t lora_write_mutex;   // guards every SPI access to the radio

    void config_lora();
    void set_lora_parameters();
    bool save_lora_config(String value);         // validate, write atomically, apply. false + alert on error
    uint8_t get_checksum(uint8_t seq, const String& data);

    // Frames and transmits one packet (blocking). false if > LORA_MAX_PAYLOAD,
    // bad type, or the radio is busy. The 2-argument form takes a new seq.
    bool LoRa_send(const String& data, uint8_t type);
    bool LoRa_send(const String& data, uint8_t type, uint8_t seq);
    void LoRa_sendAkn(uint8_t seq, uint8_t result);
    void LoRa_sendRaw(void* param);              // TX task: send_packets, ACK/retry, tx_result
    void lora_rx_task(void* param);              // RX task: FIFO read, validate, dedup, queue, ACK
    void manage_recv_queue(void* param);         // recv_packets -> WS / SSE / OLED
    void LoRa_rxMode();
    void LoRa_txMode();
    void onReceive();                            // DIO0 ISR: only notifies lora_rx_task
    void send_msg_to_events(const String& data);
    void send_msg_to_ws(const String& data);
#endif
