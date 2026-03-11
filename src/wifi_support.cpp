#include<wifi_support.h>

WiFiBackup wifi_backup;
bool WiFi_setup_done = false;
static bool sta_mode_active = false;
static TaskHandle_t wifi_monitor_handle = NULL;

void config_wifi(void *param)
{
    connect_wifi();
    WiFi_setup_done = true;
    vTaskDelete(NULL);
}

void connect_wifi()
{
    if (LittleFS.exists("/config/wifi_config.json"))
    {
        String wifi_config = get_wifi_setting("/config/wifi_config.json");
        serial_print(wifi_config);
        JsonDocument doc;
        deserializeJson(doc, wifi_config);

        const char* mode = doc["wifi_function"];
        const char* wifi_ssid = doc["wifi_ssid"];
        const char* wifi_pass = doc["wifi_pass"];

        WiFi.disconnect(true, true);
        vTaskDelay(200 / portTICK_PERIOD_MS);

        if(strcmp(mode, "AP") == 0)
        {
            serial_print("AP Mode");
            sta_mode_active = false;
            setup_ap(wifi_ssid);
        }
        else if(strcmp(mode, "STA") == 0)
        {
            serial_print("STA Mode");
            sta_mode_active = true;
            setup_sta(wifi_ssid, wifi_pass);
        }
    }
    else
    {
        sta_mode_active = false;
        setup_ap("Voyager");
    }
}

void wifi_monitor(void* param)
{
    uint32_t backoff_ms = 5000;
    const uint32_t MAX_BACKOFF_MS = 60000;

    while(true)
    {
        vTaskDelay(backoff_ms / portTICK_PERIOD_MS);

        if(WiFi_setup_done && sta_mode_active && !WiFi.isConnected())
        {
            serial_print("WiFi lost. Reconnecting (backoff=" + (String)(backoff_ms / 1000) + "s)");
            connect_wifi();
            backoff_ms = min(backoff_ms * 2, MAX_BACKOFF_MS);
        }
        else
        {
            backoff_ms = 5000; // reset on successful connection
        }
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

    // Spawn wifi_monitor only once
    if(wifi_monitor_handle == NULL || eTaskGetState(wifi_monitor_handle) == eDeleted)
    {
        xTaskCreatePinnedToCore(wifi_monitor, "wifi_monitor", 6000, NULL, 1, &wifi_monitor_handle, 1);
    }
}

void onWifiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info)
{
    serial_print("WiFi Disconnected.");
    display_buffer[1].msg = "WiFi Disconnected";
    display_buffer[2].msg = "Reconnecting...";
    display_buffer[3].msg = "";
    display_text_oled();
    // wifi_monitor handles reconnection — do NOT call WiFi functions from event callbacks
}

String get_wifi_setting(String wifi_config_path)
{
    if (LittleFS.exists(wifi_config_path))
    {
        File file = LittleFS.open(wifi_config_path);
        if(!file){
            setup_ap("Voyager");
            return "";
        }
        size_t fileSize = file.size();
        String wifi_config;
        wifi_config.reserve(fileSize + 1);
        while(file.available()){
            wifi_config += (char)file.read();
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
    File file = LittleFS.open("/config/wifi_config.json", FILE_WRITE);
    if(!file){
        Serial.println("No wifi config file present");
        return;
    }
    if(file.print(config)){
        serial_print("WiFi Config saved");
    }
    file.close();
    xTaskCreatePinnedToCore(restart, "Restart", 6000, NULL, 1, NULL, 1);
}

void setup_sta(const char* wifi_ssid, const char* wifi_pass)
{
    serial_print("Connecting to WiFi: " + (String)wifi_ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid, wifi_pass);

    display_buffer[1].msg = "WiFi Mode : STA";
    display_buffer[2].msg = wifi_ssid;

    int count = 20;
    while (!WiFi.isConnected() && count > 0)
    {
        display_buffer[3].msg = "Connecting... " + (String)count + "s";
        display_text_oled();
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        count--;
    }

    if (!WiFi.isConnected())
    {
        serial_print("STA connection failed, falling back to AP mode");
        display_buffer[2].msg = "Connect failed";
        display_buffer[3].msg = "Switching to AP";
        display_text_oled();
        sta_mode_active = false;
        String wifi_config = get_wifi_setting("/config/wifi_default.json");
        JsonDocument doc;
        deserializeJson(doc, wifi_config);
        doc["wifi_ssid"] = username;
        serializeJson(doc, wifi_config);
        doc.clear();
        save_wifi_settings(wifi_config); // internally restarts
        return;
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
    vTaskDelay(50 / portTICK_PERIOD_MS);
    vTaskDelete(NULL);
}
