#include <support_method.h>

bool lora_serial = false;
String username;
String hostname;
DNSServer dnsServer;

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
        wifi_conf["wifi_function"] = "STA";
        wifi_conf["wifi_ssid"] = wifi_ssid;
        wifi_conf["wifi_pass"] = psk;
        serializeJsonPretty(wifi_conf, wifi_config);
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
    if(strcmp(request_type, "lora_transmit") == 0)
    {
        String msg = doc["data"];
        bool get_response = doc["get_response"];
        TaskParameters* taskParams = new TaskParameters();
        taskParams->data=msg;
        xTaskCreate(LoRa_sendMessage, "LoRa_sendMessage", 12000, (void*)taskParams, 2, NULL);
        if(get_response)
            show_alert("LoRa msg transmitted successfully");
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
        send_to_ws(return_value);
    }
    if(strcmp(request_type, "set_lora_config") == 0)
    {
        String val = doc["val"];
        serial_print("changing sync word");
        serial_print(val);
        save_lora_config(val);
    }
    if(strcmp(request_type, "send_raw") == 0)
    {
        String val = doc["val"];
        serial_print(val);
        packets.push(&val);
        xTaskCreate(LoRa_sendRaw, "LoRa_sendRaw", 6000, NULL, 2, NULL);
    }
    if(strcmp(request_type, "set_serial_mode")==0)
    {
        lora_serial = doc["value"];
        if(lora_serial)
        {
            xTaskCreate(save_lora_serial_config, "save_lora_serial_config", 6000, NULL, 1, NULL);
        }
        else
        {
            xTaskCreate(save_lora_serial_config, "save_lora_serial_config", 6000, NULL, 1, NULL);
        }
    }
    if(strcmp(request_type, "get_serial_mode")==0)
    {
        JsonDocument doc;
        doc["response_type"] = "serial_mode";
        doc["value"] = lora_serial;
        String return_value;
        serializeJson(doc, return_value);
        send_to_ws(return_value);
    }
}

void save_lora_serial_config(void* param)
{
    JsonDocument doc;
    doc["lora_serial"] = lora_serial;
    String config;
    serializeJson(doc, config);
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

void serial_to_lora(void* param)
{
    while(true){
        if(lora_serial)
            if(Serial.available())
            {
                vTaskDelay(10/portTICK_PERIOD_MS);
                LoRa_txMode();
                LoRa.beginPacket();
                while(Serial.available())
                {
                    LoRa.write(Serial.read());
                }
                LoRa.endPacket(true);
                LoRa_rxMode();
            }
    }
    vTaskDelete(NULL);
}

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
        lora_serial = doc["lora_serial"];
        xTaskCreate(serial_to_lora, "serial_to_lora", 6000, NULL, 1, NULL);
    }
}

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
        const char* uname = doc["username"];
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

void save_username(String uname)
{
    serial_print("Saving username");
    serial_print(uname);
    JsonDocument doc;
    doc["username"] = uname;
    String username_config;
    serializeJson(doc, username_config);
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
    dnsServer.setTTL(300);
    dnsServer.setErrorReplyCode(DNSReplyCode::ServerFailure);
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
        Serial.println(msg);
    }
}

void setupTasks()
{
    xTaskCreate(btn_intrupt, "btn_intrupt", 6000, NULL, 2, NULL);
    xTaskCreate(led_nortifier, "led_nortifier", 6000, NULL, 1, NULL);
}

void stop_nortify_led()
{
    notify = false;
    digitalWrite(LED, LOW);
}

void nortify_led()
{
    serial_print("Nortify");
    notify = true;
}

String device_becon()
{
    String packet_type = "beacon";
    JsonDocument doc;
    doc["project"] = username;
    doc["pack_type"] = packet_type;
    String return_string;
    serializeJson(doc, return_string);
    return return_string;
}