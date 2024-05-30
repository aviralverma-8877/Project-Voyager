#ifndef web_sockets
    #define web_sockets
    #include <Arduino.h>
    #include <AsyncWebSocket.h>
    #include <support_method.h>
    #include <web_server.h>
    extern AsyncWebSocket webSocket;
    void initWebSocket();
    void handle_ws_request(void *arg, uint8_t *data, size_t len);
    void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len);
#endif