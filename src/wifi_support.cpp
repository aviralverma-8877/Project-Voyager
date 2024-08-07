#include<wifi_support.h>

WiFiBackup wifi_backup;

void config_wifi()
{
    connect_wifi();
}

void connect_wifi()
{
    if (SPIFFS.exists("/config/wifi_config.json"))
    {
        String wifi_config;
        wifi_config = get_wifi_setting();
        serial_print(wifi_config);
        JsonDocument doc;
        deserializeJson(doc, wifi_config);
        
        const char* mode = doc["wifi_function"];
        const char* wifi_ssid = doc["wifi_ssid"];
        const char* wifi_pass = doc["wifi_pass"];
        
        WiFi.disconnect(true);
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
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
        if(!WiFi.isConnected())
        {
            config_wifi();
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
    String wifi_config = get_wifi_setting();
    JsonDocument wifi_conf;
    deserializeJson(wifi_conf, wifi_config);
    wifi_conf.shrinkToFit();
    const char* ssid = wifi_conf["wifi_ssid"];
    display_buffer[2].msg = ssid;
    display_buffer[3].msg = IP.toString();
    display_text_oled();
    setup_mqtt();
    xTaskCreate(wifi_monitor, "wifi_monitor", 6000, NULL, 1, NULL);
}

void onWifiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info)
{
    serial_print("WiFi Disconnected.");
    display_buffer[1].msg = "WiFi Disconnected";
    display_buffer[2].msg = "Retrying";
    display_text_oled();
    config_wifi();
}

String get_wifi_setting()
{
    if (SPIFFS.exists("/config/wifi_config.json"))
    {
        File file = SPIFFS.open("/config/wifi_config.json");
        if(!file){
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
        Serial.println("No wifi config file present");
        return;
    }
    if(file.print(config)){
        serial_print("WiFi Config saved");
    }
    file.close();
    xTaskCreate(restart, "Restart", 6000, NULL, 1, NULL);
}

void setup_sta(const char* wifi_ssid, const char* wifi_pass)
{
    serial_print("Connecting WiFi.");
    WiFi.begin(wifi_ssid, wifi_pass);
    int count = 0;
    while (WiFi.status() != WL_CONNECTED) {
        if(count > 120)
        {
            ESP.restart();
        }
        vTaskDelay(500/portTICK_PERIOD_MS);
        serial_print(".");
        count++;
    }
    setup_mdns();
}

void setup_ap(const char* wifi_ssid)
{
    serial_print("Configuring Access Point.");
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