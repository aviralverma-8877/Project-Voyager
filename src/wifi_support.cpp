#include<wifi_support.h>

WiFiBackup wifi_backup;
bool WiFi_setup_done = false;

void config_wifi(void *param)
{
    connect_wifi();
    WiFi_setup_done = true;
    vTaskDelete(NULL);
}

void connect_wifi()
{
    if (SPIFFS.exists("/config/wifi_config.json"))
    {
        String wifi_config;
        wifi_config = get_wifi_setting("/config/wifi_config.json");
        serial_print(wifi_config);
        JsonDocument doc;
        deserializeJson(doc, wifi_config);
        
        const char* mode = doc["wifi_function"];
        const char* wifi_ssid = doc["wifi_ssid"];
        const char* wifi_pass = doc["wifi_pass"];
        
        WiFi.disconnect(true, true);
        if(strcmp(mode, "AP") == 0)
        {
            serial_print("AP Mode");
            setup_ap(wifi_ssid);
        }
        if(strcmp(mode, "STA") == 0)
        {
            serial_print("STA Mode");
            setup_sta(wifi_ssid, wifi_pass);
        }
    }
    else
    {
        setup_ap("Voyager");
    }
}

void wifi_monitor(void* param)
{
    while(true)
    {
        if(WiFi_setup_done && !WiFi.isConnected())
        {
            connect_wifi();
        }
        vTaskDelay(1000/portTICK_PERIOD_MS);
    }
}

void onWifiConnect(WiFiEvent_t event, WiFiEventInfo_t info)
{
    serial_print("WiFi Connected.");
    IPAddress IP = WiFi.localIP();
    serial_print(IP.toString());
    if(WiFi.getMode() == WIFI_MODE_AP)
        display_buffer[1].msg = "WiFi Type : AP";
    else if (WiFi.getMode() == WIFI_MODE_STA)
        display_buffer[1].msg = "WiFi Type : STA";
    String wifi_config = get_wifi_setting("/config/wifi_config.json");
    JsonDocument wifi_conf;
    deserializeJson(wifi_conf, wifi_config);
    wifi_conf.shrinkToFit();
    const char* ssid = wifi_conf["wifi_ssid"];
    display_buffer[2].msg = ssid;
    display_buffer[3].msg = IP.toString();
    display_text_oled();
    setup_mqtt();
    xTaskCreatePinnedToCore(wifi_monitor, "wifi_monitor", 6000, NULL, 1, NULL,1);
}

void onWifiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info)
{
    serial_print("WiFi Disconnected.");
    display_buffer[1].msg = "WiFi Disconnected";
    display_buffer[2].msg = "Retrying";
    display_text_oled();
    if(WiFi_setup_done)
        connect_wifi();
}

String get_wifi_setting(String wifi_config)
{
    if (SPIFFS.exists(wifi_config))
    {
        File file = SPIFFS.open(wifi_config);
        if(!file){
            serial_print("File Not found: "+wifi_config);
            setup_ap("Voyager");
            return "";
        }
        String wifi_config;
        while(file.available()){
            wifi_config += file.readString();
        }
        file.close();
        wifi_backup.backup_config = wifi_config;
        wifi_backup.backup_done = true;
        serial_print("Reading WiFi settings");
        serial_print(wifi_config);
        return wifi_config;
    }
    else{
        setup_ap("Voyager");
        return "";
    }
}

void save_wifi_settings(String config)
{
    serial_print("Saving WiFi settings");
    serial_print(config);
    wifi_backup.backup_config = config;
    wifi_backup.backup_done = true;
    File file = SPIFFS.open("/config/wifi_config.json", FILE_WRITE);
    if(!file){
        serial_print("File Not found: /config/wifi_config.json");
        Serial.println("No wifi config file present");
        return;
    }
    if(file.print(config)){
        serial_print("WiFi Config saved");
    }
    file.close();
    xTaskCreatePinnedToCore(restart, "Restart", 6000, NULL, 1, NULL,1);
}

void setup_sta(const char* wifi_ssid, const char* wifi_pass)
{
    serial_print("Connecting WiFi.");
    WiFi.begin(wifi_ssid, wifi_pass);
    vTaskDelay(1000/portTICK_PERIOD_MS);
    int count = 60;
    while (!WiFi.isConnected()) {
        if(count == 0)
        {
            String wifi_config = get_wifi_setting("/config/wifi_default.json");
            JsonDocument doc;
            deserializeJson(doc, wifi_config);
            doc["wifi_ssid"] = username;
            serializeJson(doc, wifi_config);
            doc.clear();
            save_wifi_settings(wifi_config);
            restart(NULL);
        }
        display_buffer[3].msg = "AP mode in "+(String)count;
        display_text_oled();
        vTaskDelay(1000/portTICK_PERIOD_MS);
        count--;
    }
    setup_mdns();
}

void setup_ap(const char* wifi_ssid)
{
    serial_print("Configuring Access Point.");
    const IPAddress localIP(192, 168, 4, 1);
    const IPAddress gatewayIP(192, 168, 4, 1);
    const IPAddress subnetMask(255, 255, 255, 0);
    WiFi.softAPConfig(localIP, gatewayIP, subnetMask);
    WiFi.softAP(wifi_ssid);
    IPAddress IP = WiFi.softAPIP();
    serial_print(IP.toString());
    display_buffer[1].msg = "WiFi Type : AP";
    display_buffer[2].msg = wifi_ssid;
    display_buffer[3].msg = IP.toString();
    display_text_oled();
    setup_dns();
    setup_mdns();
}

void scan_ssid(void* args)
{
    serial_print("scan_ssid");
    int n = WiFi.scanNetworks();
    serial_print("No of WiFi Network found.");
    serial_print((String)n);
    JsonDocument doc;
    doc["response_type"] = "wifi_scan";
    JsonArray array = doc["SSID"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
        JsonDocument r;
        r["ssid"] = WiFi.SSID(i);
        r["rssi"] = WiFi.RSSI(i);
        r.shrinkToFit();
        array.add(r);
    }
    String return_value;
    serializeJson(doc, return_value);
    doc.clear();
    send_to_ws(return_value);
    vTaskDelay(50/portTICK_PERIOD_MS);
    vTaskDelete(NULL);
}