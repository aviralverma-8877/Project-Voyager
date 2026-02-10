#ifndef web_sockets
    #define web_sockets
    #include <Arduino.h>
    #include <WiFi.h>
    #ifdef ESP32
        #include <AsyncTCP.h>
    #else
        #include <ESPAsyncTCP.h>
    #endif
    #include <ESPAsyncWebServer.h>
    #include <support_method.h>
    #include <web_server.h>
    #include <ArduinoJson.h>
    extern AsyncWebSocket webSocket;
    extern AsyncEventSource rawEvents;
    extern volatile uint32_t ws_client_count;
    void initWebSocket();
    void onRawEvents(AsyncEventSourceClient *client);
    void send_to_events(String data, String topic);
    void send_to_ws(String return_value);
    void handle_ws_request(void *arg, uint8_t *data, size_t len);
    void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len);
#endif