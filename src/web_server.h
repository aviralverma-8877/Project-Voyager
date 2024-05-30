#ifndef web_server
    #define web_server
    #include <Arduino.h>
    #include <ESPAsyncWebServer.h>
    #include <support_method.h>
    #include "FS.h"
    #include "SPIFFS.h"

    extern AsyncWebServer server;
    void define_api();
#endif