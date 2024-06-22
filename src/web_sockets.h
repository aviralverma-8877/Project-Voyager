#ifndef web_sockets
    #define web_sockets
    #include <Arduino.h>
    #include <AsyncWebSocket.h>
    #include <support_method.h>
    #include <web_server.h>
    #include <ArduinoJson.h>
    extern Ticker TickerForWSClientCleanup;
    extern AsyncWebSocket webSocket;
    extern bool ws_connected;
    void initWebSocket();
    void cleanup_client();
    void send_to_ws(String return_value);
    void handle_ws_request(void *arg, uint8_t *data, size_t len);
    void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len);
#endif