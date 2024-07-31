#include<lora_support.h>

bool lora_available_for_write = true;
uint8_t AknRecieved = 2;
CRC8 crc;

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
    xTaskCreate(LoRa_sendRaw,"LoRa_sendRaw", 12000, NULL, 1, NULL);
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

uint8_t get_checksum(String data)
{
    crc.restart();
    crc.add((uint8_t*)data.c_str(), data.length());
    return crc.calc();
}

void LoRa_send(String data, uint8_t type)
{
    serial_print(data);
    while(!lora_available_for_write){}
    LoRa_txMode();
    lora_available_for_write=false;
    LoRa.beginPacket();
    LoRa.write(data.length());
    LoRa.write(type);
    LoRa.write(get_checksum(data));
    for(int i=0; i<data.length(); i++)
    {
        char r = data[i];
        LoRa.write(r);
    }
    LoRa.endPacket(true);
    LoRa_rxMode();
    lora_available_for_write = true;
}

void LoRa_sendRaw(void* param) {
    serial_print("LoRa_sendRaw");
    while(true)
    {
        TaskParameters* params = new TaskParameters();
        if(xQueueReceive(send_packets, &(params) , (TickType_t)500 ))
        {
            String data = (String)params->data;
            AknRecieved = 2;
            LoRa_send(data, RAW_DATA);
            int time = millis();
            int retry = 0;
            while(AknRecieved != 1)
            {
                if(AknRecieved == 0)
                {
                    AknRecieved = 2;
                    retry += 1;
                    LoRa_send(data, RAW_DATA);
                }
                if(millis()-time > 5000)
                {
                    time = millis();
                    AknRecieved = 2;
                    retry += 1;
                    LoRa_send(data, RAW_DATA);
                }
                if(retry > 3)
                {
                    break;
                }
                vTaskDelay(50/portTICK_PERIOD_MS);
            }
        }
        free(params);
        vTaskDelay(50/portTICK_PERIOD_MS);
    }
}

void LoRa_sendMessage(void *param)  {
    TaskParameters* params = (TaskParameters*)param;
    String message = (String)params->data;
    free(params);
    serial_print("LoRa_sendMessage");
    JsonDocument doc;
    doc["name"] = username;
    doc["mac"] = WiFi.macAddress();
    doc["data"] = message;
    String lora_payload;
    serializeJson(doc, lora_payload);
    doc.clear();
    AknRecieved = 2;
    LoRa_send(lora_payload, LORA_MSG);
    int time = millis();
    int retry = 0;
    while(AknRecieved != 1)
    {
        if(AknRecieved == 0)
        {
            AknRecieved = 2;
            retry += 1;
            LoRa_send(lora_payload, LORA_MSG);
        }
        if((millis()-time) > 2000)
        {
            time = millis();
            AknRecieved = 2;
            retry += 1;
            LoRa_send(lora_payload, LORA_MSG);
        }
        if(retry > 3)
        {
            break;
        }
        vTaskDelay(50/portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

void LoRa_sendAkn(uint8_t result)
{
    serial_print("SENT AKN: "+(String)result);
    while(!lora_available_for_write){}
    LoRa_txMode();
    lora_available_for_write=false;
    LoRa.beginPacket();                   // start packet
    LoRa.write(1);                        // Dummy data length
    LoRa.write(REC_AKNG);
    LoRa.write(0);                        // Dummy Checksum
    LoRa.write(result);
    LoRa.endPacket(true);                 // finish packet and send it
    LoRa_rxMode();
    lora_available_for_write = true;
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
    else
    {
        String message;
        int size = (int)LoRa.read();
        int type = (int)LoRa.read();
        uint8_t checksum = (uint8_t)LoRa.read();
        if(type == REC_AKNG)
        {
            uint8_t result = (uint8_t)LoRa.read();
            serial_print("REC AKN: "+(String)result);
            AknRecieved=result;
            return;
        }
        for (int i=0; i<size; i++)
        {
            message += (char)LoRa.read();
        }
        if(checksum == get_checksum(message) && message.length() == size)
        {
            LoRa_sendAkn(1);
            switch (type)
            {
                case RAW_DATA:
                    send_msg_to_events(message);
                    break;
                case LORA_MSG:
                    send_msg_to_ws(message);
                    break;
                default:
                    break;                                
            }
            send_msg_to_mqtt(message);
        }
        else{
            LoRa_sendAkn(0);
        }
    }
}

void send_msg_to_mqtt(String data)
{
    JsonDocument doc;
    doc["response_type"] = "lora_rx";
    doc["lora_msg"] = data;
    String val;
    serializeJson(doc, val);
    doc.clear();
    TaskParameters* taskParams = new TaskParameters();
    taskParams->data=val;
    xTaskCreate(send_to_mqtt, "send_to_mqtt", 6000, (void*) taskParams, 1, NULL);
}

void send_msg_to_events(String data)
{
    EventParam* param = new EventParam();
    param->data = data;
    param->topic = "RAW_DATA";
    xTaskCreate(send_to_events, "send_to_events", 6000, (void*)param, 1, NULL);
}

void send_msg_to_ws(String data)
{
    JsonDocument doc;
    doc["response_type"] = "lora_rx";
    doc["lora_msg"] = data;
    String val;
    serializeJson(doc, val);
    doc.clear();
    TaskParameters* taskParams = new TaskParameters();
    taskParams->data=val;
    xTaskCreate(send_to_ws, "send_to_ws", 6000, (void*) taskParams, 1, NULL);
}

void onTxDone()
{
    serial_print("LORA packet transmitted");
}