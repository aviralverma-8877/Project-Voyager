#ifndef lora_support
    #define lora_support
    #include <Arduino.h>
    #include <SPI.h>
    #include <LoRa.h>
    #include <support_method.h>
    #include "CRC8.h"
    #include "CRC.h"
    #include "mqtt_support.h"
    #define SCK             LSCK
    #define MISO            LMISO
    #define MOSI            LMOSI
    #define SS              LNSS
    #define RST             LRST
    #define IRQ             DIO0
    #define	IMPLEMENTATION	FIFO

                                     //        Types of lora msg.
    #define LORA_MSG 0               //        The payload is normal text msg.
    #define RAW_DATA 1               //        The payload is a file chunk or raw data.
    #define REC_AKNG 2               //        The payload is an aknowledgement of recieved msg.
    #define LORA_SERIAL 3
    struct TaskParameters {
    String data;
    };

    struct QueueParam {
        int type;
        String message;
        AsyncWebServerRequest *request;
    };
    struct DebugQueueParam {
        String message;
    };
    extern CRC8 crc;
    extern bool lora_available_for_write;
    extern uint8_t AknRecieved;

    void setup_mqtt();
    void save_lora_config(String value);
    uint8_t get_checksum(String data);
    void LoRa_send(String data, uint8_t type);
    void LoRa_sendRaw(void* param);
    void set_lora_parameters();
    void config_lora();
    void LoRa_rxMode();
    void LoRa_txMode();
    void manage_recv_queue(void* param);
    void LoRa_sendAkn(uint8_t result);
    void onReceive(int packetSize);
    void onTxDone();
    void send_msg_to_events(String data);
    void send_msg_to_mqtt(String data, int type);
    void send_msg_to_ws(String data);
#endif