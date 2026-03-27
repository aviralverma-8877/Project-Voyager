#ifndef lora_support
    #define lora_support
    #include <Arduino.h>
    #include <SPI.h>
    #include <LoRa.h>
    #include <support_method.h>
    #include "CRC8.h"
    #include "CRC.h"
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
    #include "freertos/queue.h"

    #define SCK             LSCK
    #define MISO            LMISO
    #define MOSI            LMOSI
    #define SS              LNSS
    #define RST             LRST
    #define IRQ             DIO0
    #define	IMPLEMENTATION	FIFO

    // LoRa transmission constants
    #define LORA_ACK_TIMEOUT_MS    5000
    #define LORA_MAX_RETRIES       3
    #define LORA_TX_TIMEOUT_MS     10000

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
        bool send_response;  // true = send TX|OK or TX|FAIL over BT after transmission
    };

    struct DebugQueueParam {
        String message;
    };

    // FreeRTOS queue handles (created in main.cpp)
    extern QueueHandle_t serial_packet_send;
    extern QueueHandle_t serial_packet_rec;
    extern QueueHandle_t send_packets;
    extern QueueHandle_t recv_packets;
    extern QueueHandle_t debug_msg;

    extern CRC8 crc;
    extern bool lora_available_for_write;
    extern SemaphoreHandle_t lora_write_mutex;
    extern SemaphoreHandle_t ack_semaphore;
    extern volatile uint8_t AknRecieved;

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
#endif
