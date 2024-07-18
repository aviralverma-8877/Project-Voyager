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

void LoRa_sendRaw(void *param) {
    serial_print("LoRa_sendRaw");
    TaskParameters* params = (TaskParameters*)param;
    String data = (String)params->data;
    AknRecieved = 2;
    LoRa_send(data, RAW_DATA);
    while(AknRecieved != 1)
    {
        vTaskDelay(500/portTICK_PERIOD_MS);
        if(AknRecieved == 0)
        {
            AknRecieved = 2;
            LoRa_send(data, RAW_DATA);
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
    AknRecieved = 2;
    LoRa_send(lora_payload, LORA_MSG);
    while(AknRecieved != 1)
    {
        vTaskDelay(500/portTICK_PERIOD_MS);
        if(AknRecieved == 0)
        {
            AknRecieved = 2;
            LoRa_send(lora_payload, LORA_MSG);
        }
    }
    vTaskDelete(NULL);
}

void LoRa_sendAkn(void *param)
{
    AknParameters* params = (AknParameters*)param;
    uint8_t result = (uint8_t)params->result;
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
    serial_print("Akn Sent");
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
    else
    {
        String message;
        int size = LoRa.read();
        int type = LoRa.read();
        uint8_t checksum = LoRa.read();
        if(type == REC_AKNG)
        {
            uint8_t result = (uint8_t)LoRa.read();
            serial_print("Aknowledgement recieved.");
            AknRecieved=result;
            serial_print("Result: "+(String)result);
            return;
        }
        for (int i=0; i<size; i++)
        {
            message += (char)LoRa.read();
        }
        serial_print("Message Recieved: "+message);
        serial_print("Recieved Checksum: "+(String)checksum);
        serial_print("Calculated Checksum: "+(String)get_checksum(message));
        if(checksum == get_checksum(message) && message.length() == size)
        {
            AknParameters* akn_param = new AknParameters();
            akn_param->result = 1; 
            xTaskCreate(LoRa_sendAkn, "LoRa_sendAkn", 6000, (void*)akn_param, 3, NULL);
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
        else{
            AknParameters* akn_param = new AknParameters();
            akn_param->result = 0; 
            xTaskCreate(LoRa_sendAkn, "LoRa_sendAkn", 6000, (void*)akn_param, 3, NULL);
        }
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