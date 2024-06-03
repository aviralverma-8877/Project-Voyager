#include<wifi_support.h>

void config_wifi()
{
    if (SPIFFS.exists("/config/wifi_config.json"))
    {
        File file = SPIFFS.open("/config/wifi_config.json");
        if(!file){
            setup_ap("Voyager");
        }
        String wifi_config;
        while(file.available()){
            wifi_config += file.readString();
        }
        file.close();
        serial_print(wifi_config);
        JsonDocument doc;
        deserializeJson(doc, wifi_config);
        const char* mode = doc["wifi_function"];
        const char* wifi_ssid = doc["wifi_ssid"];
        const char* wifi_pass = doc["wifi_pass"];
        if(strcmp(mode, "AP") == 0)
        {
            setup_ap(wifi_ssid);
        }
        if(strcmp(mode, "STA") == 0)
        {
            setup_sta(wifi_ssid, wifi_pass);
        }
    }
    else
    {
        setup_ap("Voyager");
    }
}

void setup_sta(const char* wifi_ssid, const char* wifi_pass)
{
    serial_print("Connecting WiFi.");
    WiFi.begin(wifi_ssid, wifi_pass);
    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(500/portTICK_PERIOD_MS);
        serial_print(".");
    }
    serial_print("WiFi connected.");
    IPAddress IP = WiFi.localIP();
    serial_print(IP.toString());
    display_text_oled("WiFi Type : STA", 0, 10);
    display_text_oled(wifi_ssid, 0, 20);
    display_text_oled(IP.toString(), 0, 30);
}

void setup_ap(const char* wifi_ssid)
{
    serial_print("Configuring Access Point.");
    WiFi.softAP(wifi_ssid);
    IPAddress IP = WiFi.softAPIP();
    serial_print(IP.toString());
    display_text_oled("WiFi Type : AP", 0, 10);
    display_text_oled(wifi_ssid, 0, 20);
    display_text_oled(IP.toString(), 0, 30);
    setup_dns();
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
    for (int i = 0; i < n; ++i) {
        JsonDocument r;
        r["ssid"] = WiFi.SSID(i);
        r["rssi"] = WiFi.RSSI(i);
        array.add(r);
    }
    String return_value;
    serializeJson(doc, return_value);
    send_to_ws(return_value);
    vTaskDelete(NULL);
}