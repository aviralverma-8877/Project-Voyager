#include<wifi_support.h>

void config_ap()
{
    serial_print("Configuring Access Point.");
    WiFi.softAP(WiFI_SSID);
    IPAddress IP = WiFi.softAPIP();
    serial_print(IP.toString());
    display_text_oled("WiFi Type : AP", 0, 10);
    display_text_oled(WiFI_SSID, 0, 20);
    display_text_oled(IP.toString(), 0, 30);
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