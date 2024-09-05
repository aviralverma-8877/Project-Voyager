#ifndef support_methods
    #define support_methods
    #include <Arduino.h>
    #include <string.h>
    #include <oled_display.h>
    #include <tasks.h>
    #include <ArduinoJson.h>
    #include <lora_support.h>
    #include "SPIFFS.h"
    #define INTERRUPT_ATTR IRAM_ATTR
    extern bool lora_serial;
    extern String username;
    extern String hostname;
    extern TaskHandle_t debug_handler;
    extern QueueHandle_t send_packets;
    extern QueueHandle_t recv_packets;
    extern QueueHandle_t debug_msg;
    void serial_to_lora(void* param);
    void get_lora_serial();
    void get_username();
    void save_username(String uname);
    void restart(void *param);
    void config_gpios();
    void serial_print(String msg);
    void debugger_print(void *param);
    void setupTasks();
    void stop_transmission();
    void show_alert(String msg);
    void handle_operations(JsonDocument doc);
    void reset_device(void *param);
    void save_lora_serial_config(void* param);
    #define DEBUGGING DEBUG
#endif



