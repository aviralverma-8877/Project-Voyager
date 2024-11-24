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
    xTaskCreatePinnedToCore(LoRa_sendRaw,"LoRa_sendRaw", 6000, NULL, 1, NULL,1);
    xTaskCreatePinnedToCore(manage_recv_queue,"manage_recv_queue", 6000, NULL, 1, NULL,1);
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
        LoRa.write((char)data[i]);
    }
    LoRa.endPacket(true);
    LoRa_rxMode();
    led_nortifier();
    lora_available_for_write = true;
}

void LoRa_sendRaw(void* param) {
    serial_print("LoRa_sendRaw");
    int type, time, retry;
    while(uxQueueSpacesAvailable(send_packets) > 0 )
    {
        QueueParam* params = NULL;
        if(xQueueReceive(send_packets, &(params) , ( TickType_t )0))
        {
            type = (int)params->type;
            AknRecieved = 2;
            LoRa_send((String)params->message, type);
            time = millis();
            retry = 0;
            while(AknRecieved != 1)
            {
                if(AknRecieved == 0)
                {
                    retry += 1;
                    serial_print("Retry: "+(String)retry);
                    AknRecieved = 2;
                    LoRa_send((String)params->message, type);
                }
                if(millis()-time > 5000)
                {
                    retry += 1;
                    if(retry > 3)
                    {
                        show_alert("Transmission Failed");
                        stop_transmission();
                        break;
                    }
                    else
                    {
                        serial_print("Retry: "+(String)retry);
                        time = millis();
                        AknRecieved = 2;
                        LoRa_send((String)params->message, type);
                    }
                }
            }
        }
        else
        {
            LoRa.flush();
            xQueueReset(send_packets);
        }
        delete params;
        vTaskDelay(50/portTICK_PERIOD_MS);
    }
    show_alert("Queue is full, Rebooting...");
    stop_transmission();
    vTaskDelay(100/portTICK_PERIOD_MS);
    xTaskCreatePinnedToCore(restart,"restart",6000,NULL,1,NULL,1);
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
        led_nortifier();
        for (int i=0; i<packetSize; i++)
            Serial.write(LoRa.read());
        return;
    }
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
    serial_print(message);
    if(checksum == get_checksum(message) && message.length() == size)
    {
        QueueParam* param = new QueueParam();
        param->type = type;
        param->message = message;
        xQueueSend(recv_packets, (void*)&param, (TickType_t)50);
    }
    else{
        LoRa_sendAkn(0);
    }
}

void manage_recv_queue(void* param)
{
    int type;
    while( uxQueueSpacesAvailable( recv_packets ) > 0 )
    {
        QueueParam* params = NULL;
        if(xQueueReceive(recv_packets, &(params) , (TickType_t)0))
        {
            led_nortifier();
            type = (int)params->type;
            if(type == RAW_DATA)
            {
                send_to_ble((String)params->message);
            }
            if(type == LORA_MSG)
            {
                send_to_ble((String)params->message);
            }
        }
        else
        {
            LoRa.flush();
            xQueueReset(recv_packets);
        }
        delete params;
        vTaskDelay(50/portTICK_PERIOD_MS);
    }
    show_alert("Queue is full, Rebooting...");
    stop_transmission();
    vTaskDelay(100/portTICK_PERIOD_MS);
    xTaskCreatePinnedToCore(restart,"restart",6000,NULL,1,NULL,1);
    vTaskDelete(NULL);
}

void onTxDone()
{
    serial_print("LORA packet transmitted");
}