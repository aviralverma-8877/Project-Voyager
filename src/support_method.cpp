#include <support_method.h>

bool lora_serial = false;
String username;
String hostname;
DNSServer dnsServer;

// This is main method to handle web-socket input
// It accepts input as JsonDocument which should have the key request-type.
// Based on the request type corresponding operations will be performed on the ESP32.
void handle_operations(JsonDocument doc)
{
    const char* request_type = doc["request-type"];
    if(strcmp(request_type, "wifi_ssid_scan") == 0)
    {
        String json_string;
        xTaskCreate(scan_ssid, "scan_wifi", 6000, NULL, 1, NULL);
    }
    if(strcmp(request_type, "connect_wifi") == 0)
    {
        const char * wifi_ssid = doc["wifi_ssid"];
        serial_print(wifi_ssid);
        const char * psk = doc["wifi_pass"];
        serial_print(psk);
        // Reading wifi config
        String wifi_config = get_wifi_setting();
        // modifying wifi config
        serial_print(wifi_config);
        JsonDocument wifi_conf;
        deserializeJson(wifi_conf, wifi_config);
        wifi_conf.shrinkToFit();
        wifi_conf["wifi_function"] = "STA";
        wifi_conf["wifi_ssid"] = wifi_ssid;
        wifi_conf["wifi_pass"] = psk;
        serializeJsonPretty(wifi_conf, wifi_config);
        wifi_conf.clear();
        serial_print(wifi_config);
        // writing wifi config
        save_wifi_settings(wifi_config);
        restart(NULL);
    }
    if(strcmp(request_type, "reset_device") == 0)
    {
        xTaskCreate(reset_device, "reset_device", 6000, NULL, 1, NULL);
    }
    if(strcmp(request_type, "restart_device") == 0)
    {
        xTaskCreate(restart, "Restart", 6000, NULL, 1, NULL);
    }
    if(strcmp(request_type, "set_username") == 0)
    {
        String val = doc["val"];
        serial_print("changing username");
        serial_print(val);
        save_username(val);
    }
    if(strcmp(request_type, "get_username") == 0)
    {
        serial_print("get_username");
        JsonDocument doc;
        doc["response_type"] = "set_uname";
        doc["uname"] = username;
        String return_value;
        serializeJson(doc, return_value);
        doc.clear();
        send_to_ws(return_value);
    }
    if(strcmp(request_type, "set_mqtt_config") == 0)
    {
        String val = doc["val"];
        serial_print("saving mqtt config");
        serial_print(val);
        save_mqtt_config(val);
    }
    if(strcmp(request_type, "set_lora_config") == 0)
    {
        String val = doc["val"];
        serial_print("saving lora config");
        serial_print(val);
        save_lora_config(val);
    }
    if(strcmp(request_type, "set_serial_mode")==0)
    {
        lora_serial = doc["value"];
        xTaskCreate(save_lora_serial_config, "save_lora_serial_config", 6000, NULL, 1, NULL);
    }
    if(strcmp(request_type, "get_serial_mode")==0)
    {
        JsonDocument doc;
        doc["response_type"] = "serial_mode";
        doc["value"] = lora_serial;
        String return_value;
        serializeJson(doc, return_value);
        doc.clear();
        send_to_ws(return_value);
    }
}

// Method to save LoRa to Serial configration.
// This method should be called using xTaskCreate.
// It will save current value of lora_serial global variable to /config/lora_serial.json
void save_lora_serial_config(void* param)
{
    JsonDocument doc;
    doc["lora_serial"] = lora_serial;
    String config;
    serializeJson(doc, config);
    doc.clear();
    File file = SPIFFS.open("/config/lora_serial.json", FILE_WRITE);
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

// Loop to catch any input to Serial input and send it to lora.
// This meathod will only be enabled if lora_serial flag is true.
void serial_to_lora(void* param)
{
    while(true){
        if(lora_serial)
        {
            if(Serial.available())
            {
                String data;
                while(Serial.available())
                {
                    data += (char)Serial.read();
                }
                if(data.length() <= 200)
                {
                    QueueParam *packet = new QueueParam();
                    packet->message = data;
                    packet->type = LORA_SERIAL;
                    packet->request = NULL;
                    xQueueSend(send_packets, (void*)&packet, (TickType_t)2);
                }
            }
        }
        vTaskDelay(50/portTICK_PERIOD_MS);
    }
}

// Method to fetch the flag lora_serial.
// This method will not return any value,
// rather update the value in lora_serial flag.
void get_lora_serial()
{
    if (SPIFFS.exists("/config/lora_serial.json"))
    {
        File file = SPIFFS.open("/config/lora_serial.json");
        if(!file){
            lora_serial = false;
            return;
        }
        String lora_serial_config;
        while(file.available()){
            lora_serial_config += file.readString();
        }
        file.close();
        JsonDocument doc;
        deserializeJson(doc, lora_serial_config);
        doc.shrinkToFit();
        lora_serial = doc["lora_serial"];
        doc.clear();
        xTaskCreate(serial_to_lora, "serial_to_lora", 6000, NULL, 1, NULL);
    }
}

// Update username flag with username value
void get_username()
{
    serial_print("get_username");
    if (SPIFFS.exists("/config/user_data.json"))
    {
        File file = SPIFFS.open("/config/user_data.json");
        if(!file){
            username = WiFi.macAddress();
            return;
        }
        String username_config;
        while(file.available()){
            username_config += file.readString();
        }
        file.close();
        serial_print("Reading username");
        JsonDocument doc;
        deserializeJson(doc, username_config);
        doc.shrinkToFit();
        const char* uname = doc["username"];
        doc.clear();
        if(strcmp(uname, "")==0)
        {
            username = WiFi.macAddress();
            return;
        }
        else
        {
            username = uname;
            return;
        }
    }
}

//Send stop transmission comamnd to client
void stop_transmission()
{
    JsonDocument doc;
    doc["response_type"] = "stop_transmission";
    String return_value;
    serializeJson(doc, return_value);
    doc.clear();
    send_to_ws(return_value);
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
    File file = SPIFFS.open("/config/user_data.json", FILE_WRITE);
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

//Reset ESP32
void reset_device(void *param)
{
    show_alert("Device reset successfully,\nPlease reconfigure the device by connecting to the AP.\nRebooting Now.");
    clear_oled_display();
    display_buffer[1].msg = "Resetting";
    display_text_oled();
    serial_print("Stopping WiFi");
    if (WiFi.status() == WL_CONNECTED)
    {
        WiFi.disconnect();
    }
    serial_print("Formatting SPIFFS");
    bool formatted = SPIFFS.format();
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

void show_alert(String msg)
{
    JsonDocument doc;
    doc["response_type"] = "alert";
    doc["alert_msg"] = msg;
    String return_response;
    serializeJsonPretty(doc, return_response);
    doc.clear();
    send_to_ws(return_response);
}

void restart(void *param)
{
    vTaskDelay(500/portTICK_PERIOD_MS);
    serial_print("Restarting esp.");
    ESP.restart();
}

void setup_mdns()
{
    String mac = WiFi.macAddress();
    mac.replace(":","_");
    hostname = "project-voyager-"+mac;
    WiFi.hostname(hostname);
    serial_print("Device hostname: "+hostname);
    if (MDNS.begin(hostname.c_str())) {
        MDNS.addService("http", "tcp", 80); 
        serial_print("Started mDNS service");
    }
}

void setup_dns()
{
    serial_print("DNS service started.");
    serial_print("Soft AP IP.");
    serial_print(WiFi.softAPIP().toString());
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", WiFi.softAPIP());
    xTaskCreate(dns_request_process, "DNS Request Handler", 6000, NULL, 1, NULL);
}

void dns_request_process(void *parameter)
{
    for(;;)
    {
        dnsServer.processNextRequest();
        vTaskDelay(10/portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

void config_gpios()
{
    pinMode(LED, OUTPUT);
    pinMode(BTN1, INPUT_PULLDOWN);
    pinMode(BTN2, INPUT_PULLDOWN);
    digitalWrite(LED, LOW);
}

void serial_print(String msg)
{
    if(DEBUGGING && !lora_serial)
    {
        Serial.println("\n"+msg);
    }
}

void setupTasks()
{
    xTaskCreate(btn_intrupt, "btn_intrupt", 6000, NULL, 1, NULL);
    xTaskCreate(get_heap_info, "get_heap_info", 6000, NULL, 1, NULL);
}

void nortify_led()
{
    serial_print("Nortify");
    notify = true;
}