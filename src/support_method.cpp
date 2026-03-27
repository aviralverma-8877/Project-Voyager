#include <support_method.h>
#include "bt_support.h"

bool lora_serial = false;
String username;
TaskHandle_t debug_handler = NULL;

// Method to save LoRa to Serial configuration.
// This method should be called using xTaskCreatePinnedToCore.
// It will save current value of lora_serial global variable to /config/lora_serial.json
void save_lora_serial_config(void* param)
{
    JsonDocument doc;
    doc["lora_serial"] = lora_serial;
    String config;
    serializeJson(doc, config);
    doc.clear();
    File file = LittleFS.open("/config/lora_serial.json", FILE_WRITE);
    if(!file){
        Serial.println("No LoRa serial config file present.");
        return;
    }
    if(file.print(config)){
        serial_print("LoRa serial config saved.");
    }
    file.close();
    vTaskDelete(NULL);
}

// Forwards LoRa serial bridge received data to BT client and USB serial (debug)
void send_to_serial(void *param)
{
    while(true)
    {
        DebugQueueParam *p = NULL;
        if(xQueueReceive(serial_packet_rec, &(p), pdMS_TO_TICKS(100)) == pdTRUE)
        {
            bt_send("SERIAL|" + (String)p->message);
            if(DEBUGGING && !lora_serial)
                Serial.print((String)p->message);
            delete p;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void send_to_lora(void *param)
{
    while(true)
    {
        DebugQueueParam *p = NULL;
        if(xQueueReceive(serial_packet_send, &(p), pdMS_TO_TICKS(100)) == pdTRUE)
        {
            LoRa_send((String)p->message, LORA_SERIAL);
            delete p;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// Loop to catch any input on the USB serial and forward it to LoRa.
// Only active when lora_serial flag is true.
void serial_to_lora(void* param)
{
    Serial.flush();
    while(true){
        if(lora_serial)
        {
            if(Serial.available())
            {
                DebugQueueParam *p = new DebugQueueParam();
                if(p)
                {
                    while(Serial.available())
                        p->message += (char)Serial.read();
                    if(xQueueSend(serial_packet_send, (void*)&p, (TickType_t)2) != pdTRUE)
                        delete p;
                }
            }
        }
        else{
            Serial.flush();
        }
        vTaskDelay(10/portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

// Reads lora_serial flag from LittleFS and starts serial bridge tasks if the config file exists.
void get_lora_serial()
{
    if (LittleFS.exists("/config/lora_serial.json"))
    {
        File file = LittleFS.open("/config/lora_serial.json");
        if(!file){
            lora_serial = false;
            return;
        }
        size_t fileSize = file.size();
        String lora_serial_config;
        lora_serial_config.reserve(fileSize + 1);
        while(file.available()){
            lora_serial_config += (char)file.read();
        }
        file.close();
        JsonDocument doc;
        deserializeJson(doc, lora_serial_config);
        doc.shrinkToFit();
        lora_serial = doc["lora_serial"];
        doc.clear();
        xTaskCreatePinnedToCore(send_to_serial, "send_to_serial", 6000, NULL, 1, NULL, 1);
        xTaskCreatePinnedToCore(send_to_lora, "send_to_lora", 6000, NULL, 1, NULL, 1);
        xTaskCreatePinnedToCore(serial_to_lora, "serial_to_lora", 6000, NULL, 1, NULL, 1);
    }
}

// Load username from LittleFS; fall back to chip ID if not set
void get_username()
{
    serial_print("get_username");
    if (LittleFS.exists("/config/user_data.json"))
    {
        File file = LittleFS.open("/config/user_data.json");
        if(!file){
            // Fall back to chip ID
            uint64_t chipid = ESP.getEfuseMac();
            char id_str[13];
            snprintf(id_str, sizeof(id_str), "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
            username = String(id_str);
            return;
        }
        size_t fileSize = file.size();
        String username_config;
        username_config.reserve(fileSize + 1);
        while(file.available()){
            username_config += (char)file.read();
        }
        file.close();
        serial_print("Reading username");
        JsonDocument doc;
        deserializeJson(doc, username_config);
        doc.shrinkToFit();
        const char* uname = doc["username"];
        doc.clear();
        if(strcmp(uname, "") == 0)
        {
            uint64_t chipid = ESP.getEfuseMac();
            char id_str[13];
            snprintf(id_str, sizeof(id_str), "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
            username = String(id_str);
        }
        else
        {
            username = uname;
        }
    }
}

// Send stop transmission notification to the BT client
void stop_transmission()
{
    bt_send("STATUS|stop_transmission");
}

void save_username(String uname)
{
    serial_print("Saving username");
    serial_print(uname);
    JsonDocument doc;
    doc["username"] = uname;
    String username_config;
    serializeJson(doc, username_config);
    doc.clear();
    File file = LittleFS.open("/config/user_data.json", FILE_WRITE);
    if(!file){
        Serial.println("No username file present.");
        return;
    }
    if(file.print(username_config)){
        serial_print("Username saved");
    }
    file.close();
    username = uname;
}

// Reset ESP32 — wipes LittleFS and restarts
void reset_device(void *param)
{
    show_alert("Resetting device");
    clear_oled_display();
    display_buffer[1].msg = "Resetting";
    display_text_oled();
    serial_print("Formatting LittleFS");
    bool formatted = LittleFS.format();
    if (formatted)
    {
        serial_print("Success formatting");
        restart(NULL);
    }
    else
    {
        serial_print("Error formatting");
    }
}

// Send an alert message to the BT client
void show_alert(String msg)
{
    bt_send("STATUS|" + msg);
}

void restart(void *param)
{
    vTaskDelay(500/portTICK_PERIOD_MS);
    serial_print("Restarting esp.");
    ESP.restart();
}

void config_gpios()
{
    pinMode(LED, OUTPUT);
    digitalWrite(LED, LOW);
}

void serial_print(String msg)
{
    if(debug_handler != NULL)
    {
        DebugQueueParam *payload = new DebugQueueParam();
        if(!payload) return;
        payload->message = msg;
        if(xQueueSend(debug_msg, (void*)&payload, (TickType_t)2) != pdTRUE)
        {
            delete payload; // prevent heap exhaustion when queue is full
        }
    }
}

void debugger_print(void *param)
{
    while(true)
    {
        DebugQueueParam* params = NULL;
        if(xQueueReceive(debug_msg, &(params), pdMS_TO_TICKS(100)) == pdTRUE)
        {
            String msg = (String)params->message;
            if(DEBUGGING && !lora_serial)
            {
                Serial.println("\n"+msg);
            }
            if(bt_client_connected)
            {
                bt_send("DEBUG|" + msg);
            }
            delete params;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void setupTasks()
{
    xTaskCreatePinnedToCore(async_led_notifier, "async_led_notifier", 4000, NULL, 1, NULL, 1);
}
