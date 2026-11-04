#ifndef SUPPORT_METHOD_H
#define SUPPORT_METHOD_H
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
    #include "LittleFS.h"
    #define INTERRUPT_ATTR IRAM_ATTR
    #define DEBUGGING DEBUG

    // Written by the dispatcher / boot, read by the serial bridge and logger tasks.
    extern volatile bool lora_serial;
    extern volatile bool fs_mounted;
    extern String hostname;              // written by setup_mdns (wifi-network-engineer)
    extern DNSServer dnsServer;
    extern TaskHandle_t debug_handler;

    // Atomic config write: <path>.tmp, close, rename. Returns false on any failure
    // (the original file is left untouched).
    bool write_file_atomic(const char* path, const String& content);
    // Reads a whole file. Returns false if missing / unreadable.
    bool read_file(const char* path, String& out);

    // LoRa <-> serial bridge tasks
    void send_to_serial(void *param);
    void send_to_lora(void *param);
    void serial_to_lora(void* param);
    void start_serial_bridge();
    void get_lora_serial();
    void save_lora_serial_config(void* param);

    // Username: load_username() reads user_data.json at boot (default: MAC).
    // get_username() returns a copy under a lock; safe from any task.
    void load_username();
    String get_username();
    bool save_username(const String& uname);

    // wifi-network-engineer
    void dns_request_process(void *parameter);
    void setup_mdns();
    void setup_dns();

    void restart(void *param);
    void config_gpios();
    void serial_print(String msg);
    void debugger_print(void *param);
    void setupTasks();
    void stop_transmission();
    void show_alert(String msg);
    void handle_operations(JsonDocument& doc);
    void reset_device(void *param);
    String get_device_mode();
#endif
