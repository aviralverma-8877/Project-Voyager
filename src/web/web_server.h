// HTTP API, static UI and OTA (Contract 2). Owner: web-api-engineer.
#ifndef WEB_SERVER_H
#define WEB_SERVER_H
    #include <Arduino.h>
    #include <WiFi.h>
    #include <AsyncTCP.h>
    #include <ESPAsyncWebServer.h>
    #include <core/support_method.h>
    #include <Update.h>
    #include "FS.h"
    #include "LittleFS.h"
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
    #include "freertos/queue.h"

    #ifndef U_SPIFFS
        #define U_SPIFFS 100
    #endif
    #ifndef UPDATE_SIZE_UNKNOWN
        #define UPDATE_SIZE_UNKNOWN 0xFFFFFFFF
    #endif

    // Defined in web_server.cpp, created (and checked) in main.cpp setup().
    extern QueueHandle_t serial_packet_send;
    extern QueueHandle_t serial_packet_rec;
    extern QueueHandle_t send_packets;
    extern QueueHandle_t recv_packets;
    extern QueueHandle_t debug_msg;
    extern AsyncWebServer server;

    void firmware_web_updater();
    void define_api(void *param);   // task: registers routes, starts the server, deletes itself
    // True while a LittleFS image is being written and until the reboot after it.
    // Nothing may touch LittleFS then (HTTP answers 503, WS commands are dropped).
    bool web_fs_offline();
#endif
