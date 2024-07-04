#ifndef lora_support
    #define lora_support
    #include <Arduino.h>
    #include <SPI.h>
    #include <LoRa.h>
    #include <support_method.h>
    #include <cppQueue.h>
    #include "mqtt_support.h"
    #define SCK             LSCK
    #define MISO            LMISO
    #define MOSI            LMOSI
    #define SS              LNSS
    #define RST             LRST
    #define IRQ             DIO0
    #define	IMPLEMENTATION	LIFO

    extern cppQueue packets;
    struct TaskParameters {
    String data;
    };

    extern bool lora_available_for_write;

    void setup_mqtt();
    void save_lora_config(String value);
    void LoRa_sendRaw(void *param);
    void set_lora_parameters();
    void config_lora();
    void LoRa_rxMode();
    void LoRa_txMode();
    void LoRa_sendMessage(void *param);
    void onReceive(int packetSize);
    void onTxDone();
    void send_msg_to_mqtt( void * parameters );
    void send_msg_to_ws( void * parameter );
#endif