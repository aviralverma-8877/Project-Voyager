#include<lora_support.h>

int SyncWord = 0x34;

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
    set_lora_parameters();
    LoRa.onReceive(onReceive);
    LoRa.onTxDone(onTxDone);
    LoRa_rxMode();
}

void save_lora_config(int param, int value)
{
    File file = SPIFFS.open("/config/lora_config.json");
    if(!file){
        return;
    }
    String lora_config;
    while(file.available()){
        lora_config += file.readString();
    }
    JsonDocument doc;
    deserializeJson(doc, lora_config);
    switch (param)
    {
        case 1:
            doc["TxPower"] = value;
            break;
        case 2:
            doc["SpreadingFactor"] = value;
            break;
        case 3:
            doc["SignalBandwidth"] = value;
            break;
        case 4:
            doc["CodingRate4"] = value;
            break;
        case 5:
            doc["SyncWord"] = value;
            SyncWord = value;
            LoRa.setSyncWord(SyncWord);
        default:
            break;
    }
    deserializeJson(doc, lora_config);
    File file2 = SPIFFS.open("/config/lora_config.json", FILE_WRITE);
    if(!file2){
        Serial.println("No username file present.");
        return;
    }
    if(file2.print(lora_config)){
        serial_print("LoRa config saved");
    }
    file2.close();
}

void set_lora_parameters()
{
    if (SPIFFS.exists("/config/lora_config.json"))
    {
        File file = SPIFFS.open("/config/lora_config.json");
        if(!file){
            return;
        }
        String lora_config;
        while(file.available()){
            lora_config += file.readString();
        }
        file.close();
        serial_print(lora_config);
        JsonDocument doc;
        deserializeJson(doc, lora_config);
        int TxPower = doc["TxPower"];
        int SpreadingFactor = doc["SpreadingFactor"];
        int SignalBandwidth = doc["SignalBandwidth"];
        int CodingRate = doc["CodingRate4"];
        SyncWord = doc["SyncWord"];
        LoRa.setTxPower(TxPower);
        // LoRa.setSpreadingFactor(SpreadingFactor);
        // LoRa.setSignalBandwidth(SignalBandwidth);
        // LoRa.setCodingRate4(CodingRate);
        LoRa.setSyncWord(SyncWord);
    }
}

void enable_LoRa_file_tx_mode()
{
    TickerForLoraBeacon.detach();
    LoRa_txMode();
}

void disable_LoRa_file_tx_mode()
{
    LoRa_rxMode();
    TickerForLoraBeacon.attach(10, transmit_beacon);
}

void enable_LoRa_file_rx_mode()
{
    TickerForLoraBeacon.detach();
    LoRa_rxMode();
}

void disable_LoRa_file_rx_mode()
{
    TickerForLoraBeacon.attach(10, transmit_beacon);
}

void LoRa_rxMode(){
    LoRa.enableInvertIQ();                // active invert I and Q signals
    LoRa.receive();                       // set receive mode
}

void LoRa_txMode(){
    LoRa.idle();                          // set standby mode
    LoRa.disableInvertIQ();               // normal mode
}

void LoRa_sendRaw(String data) {
    LoRa.beginPacket();
    LoRa.print(data);
    LoRa.endPacket(true);
}

void LoRa_sendMessage(String message) {
    JsonDocument doc;
    doc["name"] = username;
    doc["mac"] = WiFi.macAddress();
    doc["data"] = message;
    String lora_payload;
    serializeJson(doc, lora_payload);
    LoRa_txMode();                        // set tx mode
    LoRa.beginPacket();                   // start packet
    LoRa.print(lora_payload);                  // add payload
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