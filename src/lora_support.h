#ifndef lora_support
    #define lora_support
    #include <Arduino.h>
    #include <SPI.h>
    #include <LoRa.h>
    #include <support_method.h>
    #include "mqtt_support.h"
    #define SCK             LSCK
    #define MISO            LMISO
    #define MOSI            LMOSI
    #define SS              LNSS
    #define RST             LRST
    #define FREQ            433E6
    #define IRQ             DIO0
    extern int SyncWord;
    void setup_mqtt();
    void save_lora_config(int param, int value);
    void enable_LoRa_file_tx_mode();
    void disable_LoRa_file_tx_mode();
    void enable_LoRa_file_rx_mode();
    void disable_LoRa_file_rx_mode();
    void LoRa_sendRaw(String data);
    void set_lora_parameters();
    void config_lora();
    void LoRa_rxMode();
    void LoRa_txMode();
    void LoRa_sendMessage(String message);
    void onReceive(int packetSize);
    void onTxDone();
    void send_msg_to_mqtt( void * parameters );
    void send_msg_to_ws( void * parameter );
#endif