#ifndef support_methods
    #define support_methods
    #include <Arduino.h>
    #include <Ticker.h>
    #include <string.h>
    #include <oled_display.h>
    #include <tasks.h>
    #include <ESPmDNS.h>
    #include <DNSServer.h>
    #include <WiFi.h>
    #include <ArduinoJson.h>
    #include <lora_support.h>
    #include <wifi_support.h>
    #include "SPIFFS.h"
    extern String username;
    extern String hostname;
    extern DNSServer dnsServer;
    extern Ticker TickerForLedNotification;
    extern Ticker TickerForLoraBeacon;

    void get_username();
    void save_username(String uname);
    void dns_request_process();
    void setup_mdns();
    void setup_dns();
    void restart();
    void config_gpios();
    void serial_print(String msg);
    void setupTickers();
    void stop_nortify_led();
    void nortify_led();
    void show_alert(String msg);
    void handle_operations(JsonDocument doc);
    void reset_device();
    String device_becon();
    #define DEBUGGING DEBUG
#endif



