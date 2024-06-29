#ifndef web_server
    #define web_server
    #include <Arduino.h>
    #include <AsyncTCP.h>
    #include <ESPAsyncWebServer.h>
    #include <support_method.h>
    #include <Update.h>
    #include "FS.h"
    #include "SPIFFS.h"

    #define U_FLASH   0
    #define U_SPIFFS  100
    #define UPDATE_SIZE_UNKNOWN 0xFFFFFFFF

    extern AsyncWebServer server;
    void firmware_web_updater();
    void define_api();
#endif