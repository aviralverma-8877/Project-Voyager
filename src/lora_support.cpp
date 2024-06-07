#include<lora_support.h>
struct TaskParameters {
  String data;
};
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
    LoRa_rxMode();
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
    LoRa_rxMode();
}

void onReceive(int packetSize)
{
    String message;
    message = "";
    for (int i = 0; i < packetSize; i++) {
        message += (char)LoRa.read();
    }
    serial_print("LoRa message received: ");
    serial_print(message);
    TaskParameters* taskParams = new TaskParameters();
    taskParams->data=message;
    xTaskCreate(send_msg_to_ws, "lora message to ws", 6000, (void*)taskParams, 0, NULL);
}

void send_msg_to_ws( void * parameters )
{
    TaskParameters* params = (TaskParameters*)parameters;
    JsonDocument doc;
    doc["response_type"] = "lora_rx";
    doc["lora_msg"] = params->data;
    String data;
    serializeJson(doc, data);
    send_to_ws(data);
    vTaskDelete(NULL);
}

void onTxDone()
{
    serial_print("LORA packet transmitted");
}