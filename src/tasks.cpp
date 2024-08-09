#include <tasks.h>

bool notify = false;
bool btn_1_pressed = false;
bool btn_2_pressed = false;

void wifi_connection_check()
{
    if(WiFi.status() != WL_CONNECTED)
    {
        config_wifi();
    }
}

void ping_mqtt_timer(void *param)
{
    String mac, mqtt_ping;
    JsonDocument doc;
    while(true)
    {
        if(mqttClient.connected())
        {
            mac = WiFi.macAddress();
            doc["mac"] = mac;
            doc["uname"] = username;
            serializeJson(doc, mqtt_ping);
            doc.clear();
            ping_mqtt(mqtt_ping);
        }
        vTaskDelay(MQTT_PING_TIME/portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

void led_nortifier()
{
    if(digitalRead(LED))
    {
        digitalWrite(LED, LOW);
        vTaskDelay(NOTIFY_LED_DELAY/portTICK_PERIOD_MS);
        digitalWrite(LED, HIGH);
    }
    else
    {
        digitalWrite(LED, HIGH);
        vTaskDelay(NOTIFY_LED_DELAY/portTICK_PERIOD_MS);
        digitalWrite(LED, LOW);
    }
}

void btn_intrupt(void *param)
{
    while(true)
    {
        if(digitalRead(BTN1) == STATE)
        {
            if(!btn_1_pressed)
            {
                btn_1_pressed = true;
                serial_print("BUTTON 1 Pressed");
                JsonDocument doc;
                doc["name"] = username;
                doc["mac"] = WiFi.macAddress();

                JsonDocument msg;
                msg["pack_type"] = "action";
                msg["data"] = "sos";
                msg.shrinkToFit();
                String msg_string;
                serializeJson(msg, msg_string);
                msg.clear();

                doc["data"] = msg_string;
                String lora_payload;
                serializeJson(doc, lora_payload);
                doc.clear();
                QueueParam* taskParams = new QueueParam();
                taskParams->message=lora_payload;
                taskParams->type = LORA_MSG;
                taskParams->request = NULL;
                xQueueSend(send_packets, (void*)&taskParams, (TickType_t)2);
            }
        }
        else
        {
            if(btn_1_pressed)
            {
                serial_print("BUTTON 1 Released");
                btn_1_pressed = false;
            }
        }
        
        if(digitalRead(BTN2) == STATE)
        {
            if(!btn_2_pressed)
            {
                btn_2_pressed = true;
                serial_print("BUTTON 2 Pressed");
            }
        }
        else
        {
            if(btn_2_pressed)
            {
                serial_print("BUTTON 2 Released");
                btn_2_pressed = false;
            }
        }
        vTaskDelay(100/portTICK_PERIOD_MS);
    }
}

void get_heap_info(void* params)
{
    int free_heap, heap_size;
    JsonDocument doc;
    String data;
    while(true)
    {
        free_heap = ESP.getFreeHeap();
        heap_size = ESP.getHeapSize();
        doc["free_heap"] = free_heap;
        doc["heap_size"] = heap_size;
        serializeJson(doc, data);
        doc.clear();
        send_to_events(data, "RAM_DATA");
        vTaskDelay(1000/portTICK_PERIOD_MS);
    }
}