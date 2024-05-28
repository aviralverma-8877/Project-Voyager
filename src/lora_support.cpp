#include<lora_support.h>

void config_lora()
{
    serial_print("Configuring LORA");
    SPI.begin(SCK, MISO, MOSI, SS);
    LoRa.setPins(SS, RST, IRQ);
    if (!LoRa.begin(FREQ)) {
        serial_print("LoRa init failed. Check your connections.");
        while (true);
    }
    LoRa.onReceive(onReceive);
    LoRa.onTxDone(onTxDone);
}

void LoRa_rxMode(){
    LoRa.enableInvertIQ();                // active invert I and Q signals
    LoRa.receive();                       // set receive mode
}

void LoRa_txMode(){
    LoRa.idle();                          // set standby mode
    LoRa.disableInvertIQ();               // normal mode
}

void LoRa_sendMessage(String message) {
    LoRa_txMode();                        // set tx mode
    LoRa.beginPacket();                   // start packet
    LoRa.print(message);                  // add payload
    LoRa.endPacket(true);                 // finish packet and send it
}

void onReceive(int packetSize)
{
    String message = "";
    while (LoRa.available()) {
        message += (char)LoRa.read();
    }
    serial_print("Node Receive: ");
    serial_print(message);
}

void onTxDone()
{
    serial_print("LORA packet transmitted");
    LoRa_rxMode();
}