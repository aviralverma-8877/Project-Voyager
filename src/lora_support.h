#ifndef lora_support
    #define lora_support
    #include <Arduino.h>
    #include <SPI.h>
    #include <LoRa.h>
    #include <support_method.h>
#endif

#define SCK             LSCK
#define MISO            LMISO
#define MOSI            LMOSI
#define SS              LNSS
#define RST             LRST
#define FREQ            433E6
#define IRQ             DIO0

void config_lora();
void LoRa_rxMode();
void LoRa_txMode();
void LoRa_sendMessage(String message);
void onReceive(int packetSize);
void onTxDone();
