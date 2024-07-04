#include<lora_support.h>

bool lora_available_for_write = true;

void config_lora()
{
    serial_print("Configuring LORA");
    SPI.begin(SCK, MISO, MOSI, SS);
    LoRa.setPins(SS, RST, IRQ);
    if (!LoRa.begin(433E6)) {
        serial_print("LoRa init failed. Check your connections.");
        while (true);
    }
    set_lora_parameters();
    LoRa.onReceive(onReceive);
    LoRa.onTxDone(onTxDone);
    LoRa_rxMode();
}

void save_lora_config(String value)
{
    if (SPIFFS.exists("/config/lora_config.json"))
    {
        File file = SPIFFS.open("/config/lora_config.json", FILE_WRITE);
        if(!file){
            return;
        }
        if(file.print(value)){
            serial_print("LoRa config saved");
        }
        JsonDocument doc;
        deserializeJson(doc, value);

        int lora_freq = doc["freq"];
        LoRa.setFrequency(lora_freq);

        int TxPower = doc["TxPower"];
        LoRa.setTxPower(TxPower);

        int SpreadingFactor = doc["SpreadingFactor"];
        LoRa.setSpreadingFactor(SpreadingFactor);

        int SignalBandwidth = doc["SignalBandwidth"];
        LoRa.setSignalBandwidth(SignalBandwidth);

        int CodingRate = doc["CodingRate4"];
        LoRa.setCodingRate4(CodingRate);

        int SyncWord = doc["SyncWord"];
        LoRa.setSyncWord(SyncWord);

        show_alert("LoRa config saved successfully");
    }
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
        int freq = doc["freq"];
        int TxPower = doc["TxPower"];
        int SpreadingFactor = doc["SpreadingFactor"];
        int SignalBandwidth = doc["SignalBandwidth"];
        int CodingRate = doc["CodingRate4"];
        int SyncWord = doc["SyncWord"];
        LoRa.setFrequency(freq);
        LoRa.setTxPower(TxPower);
        LoRa.setSpreadingFactor(SpreadingFactor);
        LoRa.setSignalBandwidth(SignalBandwidth);
        LoRa.setCodingRate4(CodingRate);
        LoRa.setSyncWord(SyncWord);
    }
}

void enable_LoRa_file_tx_mode()
{
    LoRa_txMode();
}

void disable_LoRa_file_tx_mode()
{
    LoRa_rxMode();
}

void enable_LoRa_file_rx_mode()
{
    LoRa_rxMode();
}

void disable_LoRa_file_rx_mode()
{
}

void LoRa_rxMode(){
    LoRa.enableInvertIQ();                // active invert I and Q signals
    LoRa.receive();                       // set receive mode
}

void LoRa_txMode(){
    LoRa.idle();                          // set standby mode
    LoRa.disableInvertIQ();               // normal mode
}

void LoRa_sendRaw(void *param) {
    serial_print("LoRa_sendRaw");
    TaskParameters* params = (TaskParameters*)param;
    String data = params->data;
    while(!lora_available_for_write){}
    lora_available_for_write=false;
    LoRa.flush();
    LoRa.beginPacket();
    LoRa.write(data.length());
    for(int i=0; i<data.length(); i++)
    {
        char r = data[i];
        LoRa.write(r);
    }
    LoRa.endPacket(true);
    LoRa.flush();
    lora_available_for_write = true;
    vTaskDelete(NULL);
}

void LoRa_sendMessage(String message) {
    serial_print("LoRa_sendMessage");
    JsonDocument doc;
    doc["name"] = username;
    doc["mac"] = WiFi.macAddress();
    doc["data"] = message;
    String lora_payload;
    serializeJson(doc, lora_payload);
    LoRa_txMode();                        // set tx mode
    while(!lora_available_for_write){}
    lora_available_for_write=false;
    LoRa.flush();
    LoRa.beginPacket();                   // start packet
    LoRa.write(lora_payload.length());
    for(int i=0; i<lora_payload.length(); i++)
    {
        char r = lora_payload[i];
        LoRa.write(r);
    }
    LoRa.endPacket(true);                 // finish packet and send it
    LoRa.flush();
    lora_available_for_write = true;
    LoRa_rxMode();
}

void onReceive(int packetSize)
{
    if(lora_serial)
    {
        for (int i=0; i<packetSize; i++)
        {
            Serial.write(LoRa.read());
        }        
    }
    else{
        String message;
        int size = LoRa.read();
        serial_print((String)size);
        for (int i=0; i<size; i++)
        {
            message += (char)LoRa.read();
        }
        TaskParameters* taskParams = new TaskParameters();
        taskParams->data=message;
        xTaskCreate(send_msg_to_ws, "lora message to ws", 12000, (void*)taskParams, 1, NULL);
        xTaskCreate(send_msg_to_mqtt, "lora message to mqtt", 12000, (void*)taskParams, 1, NULL);
    }
    LoRa.flush();
}

void send_msg_to_mqtt( void * parameters )
{
    TaskParameters* params = (TaskParameters*)parameters;
    JsonDocument doc;
    doc["response_type"] = "lora_rx";
    doc["lora_msg"] = (String)params->data;
    String data;
    serializeJson(doc, data);
    doc.clear();
    send_to_mqtt(data);
    vTaskDelete(NULL);
}

void send_msg_to_ws( void * parameters )
{
    TaskParameters* params = (TaskParameters*)parameters;
    JsonDocument doc;
    doc["response_type"] = "lora_rx";
    doc["lora_msg"] = (String)params->data;
    String data;
    serializeJson(doc, data);
    doc.clear();
    send_to_ws(data);
    vTaskDelete(NULL);
}

void onTxDone()
{
    serial_print("LORA packet transmitted");
}