#ifndef support_methods
    #define support_methods
    #include <Arduino.h>
    #include <string.h>
    #include <oled_display.h>
    #include <tasks.h>
    #include <ESPmDNS.h>
    #include <DNSServer.h>
    #include <WiFi.h>
    #include <ArduinoJson.h>
    #include <lora_support.h>
    #include <wifi_support.h>
    #include <lora_support.h>
    #include "SPIFFS.h"
    extern bool lora_serial;
    extern String username;
    extern String hostname;
    extern DNSServer dnsServer;

    void serial_to_lora(void* param);
    void get_lora_serial();
    void get_username();
    void save_username(String uname);
    void dns_request_process(void *parameter);
    void setup_mdns();
    void setup_dns();
    void restart(void *param);
    void config_gpios();
    void serial_print(String msg);
    void setupTasks();
    void stop_transmission();
    void nortify_led();
    void show_alert(String msg);
    void handle_operations(JsonDocument doc);
    void reset_device(void *param);
    void save_lora_serial_config(void* param);
    #define DEBUGGING DEBUG
#endif



