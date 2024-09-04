#ifndef web_server
    #define web_server
    #include <Arduino.h>
    #include <WiFi.h>

    #ifdef ESP32
        #include <AsyncTCP.h>
    #else
        #include <ESPAsyncTCP.h>
    #endif
    #include <ESPAsyncWebServer.h>
    #include <support_method.h>
    #include <Update.h>
    #include "FS.h"
    // #include "SPIFFS.h"
    #include <LITTLEFS.h>
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
    #include "freertos/queue.h"

    #define U_FLASH   0
    #define U_SPIFFS  100
    #define UPDATE_SIZE_UNKNOWN 0xFFFFFFFF
    extern QueueHandle_t send_packets;
    extern QueueHandle_t recv_packets;
    extern QueueHandle_t debug_msg;
    extern AsyncWebServer server;
    void firmware_web_updater();
    void define_api(void *param);
#endif