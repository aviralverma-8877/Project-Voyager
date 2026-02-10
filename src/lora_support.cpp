#include<lora_support.h>

bool lora_available_for_write = true;
volatile uint8_t AknRecieved = 2;
CRC8 crc;
SemaphoreHandle_t lora_write_mutex = NULL;
SemaphoreHandle_t ack_semaphore = NULL;

void config_lora()
{
    serial_print("Configuring LORA");

    // Create synchronization primitives
    lora_write_mutex = xSemaphoreCreateMutex();
    ack_semaphore = xSemaphoreCreateBinary();

    if(lora_write_mutex == NULL || ack_semaphore == NULL) {
        serial_print("Failed to create LORA synchronization primitives");
        while (true);
    }

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
    if (LittleFS.exists("/config/lora_config.json"))
    {
        File file = LittleFS.open("/config/lora_config.json", FILE_WRITE);
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
    if (LittleFS.exists("/config/lora_config.json"))
    {
        File file = LittleFS.open("/config/lora_config.json");
        if(!file){
            return;
        }
        size_t fileSize = file.size();
        String lora_config;
        lora_config.reserve(fileSize + 1);
        while(file.available()){
            lora_config += (char)file.read();
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

    // Use mutex with timeout to prevent deadlock
    if(xSemaphoreTake(lora_write_mutex, pdMS_TO_TICKS(LORA_TX_TIMEOUT_MS)) != pdTRUE) {
        serial_print("ERROR: LoRa write mutex timeout");
        return;
    }

    LoRa_txMode();
    LoRa.beginPacket();
    LoRa.write(data.length());
    LoRa.write(type);
    LoRa.write(get_checksum(data));

    // Write data efficiently
    const char* dataPtr = data.c_str();
    for(int i=0; i<data.length(); i++)
    {
        LoRa.write(dataPtr[i]);
    }

    LoRa.endPacket(true);
    LoRa_rxMode();
    led_nortifier();

    xSemaphoreGive(lora_write_mutex);
}

void LoRa_sendRaw(void* param) {
    serial_print("LoRa_sendRaw");
    int type, retry;

    while(true)
    {
        QueueParam* params = NULL;

        // Wait for items in queue with timeout to prevent reboot
        if(xQueueReceive(send_packets, &(params), pdMS_TO_TICKS(100)) == pdTRUE)
        {
            type = (int)params->type;
            retry = 0;
            bool transmission_success = false;

            // Clear any pending ACK semaphore
            xSemaphoreTake(ack_semaphore, 0);

            // Retry loop with proper timeout handling
            while(retry <= LORA_MAX_RETRIES)
            {
                AknRecieved = 2; // Reset ACK state
                LoRa_send((String)params->message, type);

                // Wait for ACK using semaphore with timeout (non-blocking)
                if(xSemaphoreTake(ack_semaphore, pdMS_TO_TICKS(LORA_ACK_TIMEOUT_MS)) == pdTRUE)
                {
                    // ACK received, check result
                    if(AknRecieved == 1)
                    {
                        transmission_success = true;
                        break;
                    }
                    else if(AknRecieved == 0)
                    {
                        // NACK received, retry
                        retry++;
                        serial_print("NACK received, Retry: "+(String)retry);
                    }
                }
                else
                {
                    // Timeout occurred
                    retry++;
                    serial_print("ACK timeout, Retry: "+(String)retry);
                }

                // Yield to other tasks between retries
                vTaskDelay(10/portTICK_PERIOD_MS);
            }

            // Handle transmission result
            if(!transmission_success)
            {
                show_alert("Transmission Failed after "+(String)LORA_MAX_RETRIES+" retries");
                stop_transmission();
            }

            // Send HTTP response if requested
            if(params->request != NULL)
            {
                JsonDocument doc;
                doc["akn"] = transmission_success ? 1 : 0;
                doc["username"] = username;
                doc.shrinkToFit();
                String return_res;
                serializeJson(doc, return_res);
                doc.clear();
                int return_code = transmission_success ? 200 : 422;
                params->request->send(return_code, "text/json", return_res);
            }

            delete params;
        }

        // Yield to prevent task starvation
        vTaskDelay(10/portTICK_PERIOD_MS);
    }
}

void LoRa_sendAkn(uint8_t result)
{
    serial_print("SENT AKN: "+(String)result);

    // Use mutex with timeout
    if(xSemaphoreTake(lora_write_mutex, pdMS_TO_TICKS(LORA_TX_TIMEOUT_MS)) != pdTRUE) {
        serial_print("ERROR: LoRa ACK mutex timeout");
        return;
    }

    LoRa_txMode();
    LoRa.beginPacket();
    LoRa.write(1);                        // Dummy data length
    LoRa.write(REC_AKNG);
    LoRa.write(0);                        // Dummy Checksum
    LoRa.write(result);
    LoRa.endPacket(true);
    LoRa_rxMode();

    xSemaphoreGive(lora_write_mutex);
}

void onReceive(int packetSize)
{
    uint8_t size = (uint8_t)LoRa.read();
    uint8_t type = (uint8_t)LoRa.read();
    uint8_t checksum = (uint8_t)LoRa.read();

    if(type == LORA_SERIAL)
    {
        DebugQueueParam *p = new DebugQueueParam();
        p->message.reserve(size + 1);
        while(LoRa.available())
            p->message +=(char)LoRa.read();
        xQueueSend(serial_packet_rec, (void*)&p, (TickType_t)2);
        return;
    }

    if(type == REC_AKNG)
    {
        uint8_t result = (uint8_t)LoRa.read();
        serial_print("REC AKN: "+(String)result);
        AknRecieved = result;

        // Signal ACK reception via semaphore (safe from ISR)
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(ack_semaphore, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        return;
    }

    // Pre-allocate message buffer
    String message;
    message.reserve(size + 1);
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
        param->request = NULL;

        // Try to send to queue, but don't block in ISR
        BaseType_t result = xQueueSendFromISR(recv_packets, (void*)&param, NULL);
        if(result != pdTRUE) {
            // Queue full, delete param to prevent memory leak
            delete param;
            serial_print("WARNING: recv_packets queue full, dropping packet");
            LoRa_sendAkn(0);
        } else {
            LoRa_sendAkn(1);
        }
    }
    else{
        serial_print("ERROR: Checksum mismatch or size mismatch");
        LoRa_sendAkn(0);
    }
}

void manage_recv_queue(void* param)
{
    int type;
    uint32_t queue_full_count = 0;
    const uint32_t QUEUE_FULL_THRESHOLD = 10;

    while(true)
    {
        QueueParam* params = NULL;

        // Wait for items with timeout
        if(xQueueReceive(recv_packets, &(params), pdMS_TO_TICKS(100)) == pdTRUE)
        {
            led_nortifier();
            type = (int)params->type;

            if(type == RAW_DATA)
            {
                send_msg_to_events((String)params->message);
            }
            if(type == LORA_MSG)
            {
                send_msg_to_ws((String)params->message);
            }
            send_msg_to_mqtt((String)params->message, type);

            delete params;
            queue_full_count = 0; // Reset counter on successful processing
        }
        else
        {
            // Check if queue is critically full
            UBaseType_t spaces = uxQueueSpacesAvailable(recv_packets);
            if(spaces == 0)
            {
                queue_full_count++;
                serial_print("WARNING: recv_packets queue full (count: "+(String)queue_full_count+")");

                if(queue_full_count >= QUEUE_FULL_THRESHOLD)
                {
                    serial_print("ERROR: Queue persistently full, applying backpressure");
                    show_alert("System overload, dropping old packets");

                    // Drop oldest packets to make room (backpressure)
                    QueueParam* dropped = NULL;
                    while(uxQueueSpacesAvailable(recv_packets) < 5) {
                        if(xQueueReceive(recv_packets, &(dropped), 0) == pdTRUE) {
                            delete dropped;
                        }
                    }
                    queue_full_count = 0;
                }
            }
        }

        // Yield to prevent task starvation
        vTaskDelay(10/portTICK_PERIOD_MS);
    }
}

void send_msg_to_mqtt(String data, int type)
{
    JsonDocument doc;
    doc["response_type"] = "lora_rx";
    doc["lora_msg"] = data;
    doc.shrinkToFit();
    String val;
    String mac = WiFi.macAddress();
    String topic;
    if(type == RAW_DATA)
        topic = "voyager/"+mac+"/"+mqtt_topic_to_publish+"/RAW_DATA";
    if(type == LORA_MSG)
        topic = "voyager/"+mac+"/"+mqtt_topic_to_publish+"/LORA_MSG";
    serializeJson(doc, val);
    doc.clear();
    send_to_mqtt(val, topic);
}

void send_msg_to_events(String data)
{
    JsonDocument doc;
    doc["millis"] = millis();
    doc["data"] = data;
    String value;
    serializeJson(doc, value);
    doc.clear();
    send_to_events(value, "RAW_DATA");
}

void send_msg_to_ws(String data)
{
    JsonDocument doc;
    doc["response_type"] = "lora_rx";
    doc["lora_msg"] = data;
    String val;
    serializeJson(doc, val);
    doc.clear();
    send_to_ws(val);
}
void onTxDone()
{
    serial_print("LORA packet transmitted");
}