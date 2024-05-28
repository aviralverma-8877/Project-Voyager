#include<wifi_support.h>

void config_ap()
{
    serial_print("Configuring Access Point.");
    WiFi.softAP(SSID);
    IPAddress IP = WiFi.softAPIP();
    serial_print(IP.toString());
    display_text_oled("WiFi Type : AP", 0, 10);
    display_text_oled(SSID, 0, 20);
    display_text_oled(IP.toString(), 0, 30);
}