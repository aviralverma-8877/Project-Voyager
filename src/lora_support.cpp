#include<lora_support.h>

cppQueue packets(sizeof(String), 10, IMPLEMENTATION);
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
        doc.shrinkToFit();
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
        doc.clear();
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
        doc.shrinkToFit();
        int freq = doc["freq"];
        int TxPower = doc["TxPower"];
        int SpreadingFactor = doc["SpreadingFactor"];
        int SignalBandwidth = doc["SignalBandwidth"];
        int CodingRate = doc["CodingRate4"];
        int SyncWord = doc["SyncWord"];
        doc.clear();
        LoRa.setFrequency(freq);
        LoRa.setTxPower(TxPower);
        LoRa.setSpreadingFactor(SpreadingFactor);
        LoRa.setSignalBandwidth(SignalBandwidth);
        LoRa.setCodingRate4(CodingRate);
        LoRa.setSyncWord(SyncWord);
    }
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
    if(packets.getCount()>0)
    {
        while(packets.getCount()>0)
        {
            String data;
            packets.pop(&data);
            LoRa_txMode();
            while(!lora_available_for_write){}
            lora_available_for_write=false;
            LoRa.beginPacket();
            LoRa.write(data.length());
            LoRa.write(RAW_DATA);
            for(int i=0; i<data.length(); i++)
            {
                char r = data[i];
                LoRa.write(r);
            }
            LoRa.endPacket(true);
            LoRa_rxMode();
            vTaskDelay(500/portTICK_PERIOD_MS);
            lora_available_for_write = true;
        }
    }
    vTaskDelete(NULL);
}

void LoRa_sendMessage(void *param)  {
    TaskParameters* params = (TaskParameters*)param;
    String message = (String)params->data;
    serial_print("LoRa_sendMessage");
    JsonDocument doc;
    doc["name"] = username;
    doc["mac"] = WiFi.macAddress();
    doc["data"] = message;
    String lora_payload;
    serializeJson(doc, lora_payload);
    doc.clear();
    LoRa_txMode();                        // set tx mode
    while(!lora_available_for_write){}
    lora_available_for_write=false;
    LoRa.beginPacket();                   // start packet
    LoRa.write(lora_payload.length());
    LoRa.write(LORA_MSG);
    for(int i=0; i<lora_payload.length(); i++)
    {
        char r = lora_payload[i];
        LoRa.write(r);
    }
    LoRa.endPacket(true);                 // finish packet and send it
    LoRa_rxMode();
    vTaskDelay(10/portTICK_PERIOD_MS);
    lora_available_for_write = true;
    vTaskDelete(NULL);
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
        int type = LoRa.read();
        serial_print((String)size);
        for (int i=0; i<size; i++)
        {
            message += (char)LoRa.read();
        }
        TaskParameters* taskParams = new TaskParameters();
        taskParams->data=message;
        switch (type)
        {
            case RAW_DATA:
                xTaskCreate(send_msg_to_events, "lora message to ws", 20000, (void*)taskParams, 1, NULL);
                break;
            case LORA_MSG:
                xTaskCreate(send_msg_to_ws, "lora message to ws", 20000, (void*)taskParams, 1, NULL);
                break;      
            default:
                break;
        }
        xTaskCreate(send_msg_to_mqtt, "lora message to mqtt", 20000, (void*)taskParams, 1, NULL);
    }
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

void send_msg_to_events(void * parameters)
{
    TaskParameters* params = (TaskParameters*)parameters;
    String data = (String)params->data;
    send_to_events(data.c_str(), "RAW_DATA");
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