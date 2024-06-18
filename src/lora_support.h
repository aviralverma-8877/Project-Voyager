#ifndef lora_support
    #define lora_support
    #include <Arduino.h>
    #include <SPI.h>
    #include <LoRa.h>
    #include <support_method.h>
    #define SCK             LSCK
    #define MISO            LMISO
    #define MOSI            LMOSI
    #define SS              LNSS
    #define RST             LRST
    #define FREQ            433E6
    #define IRQ             DIO0
    extern int SyncWord;
    void save_lora_config(int param, int value);
    void set_lora_parameters();
    void config_lora();
    void LoRa_rxMode();
    void LoRa_txMode();
    void LoRa_sendMessage(String message);
    void onReceive(int packetSize);
    void onTxDone();
    void send_msg_to_ws( void * parameter );
#endif