#ifndef wifi_support
    #define wifi_support
    #include <Arduino.h>
    #include <WiFi.h>
    #include <support_method.h>
    #include <ArduinoJson.h>
    #include <web_sockets.h>
#endif

#define WiFI_SSID "ESP32_LORA"
void config_ap();
void scan_ssid(void* args);