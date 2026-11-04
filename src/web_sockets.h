// WebSocket /ws and SSE /rawEvents transport (Contracts 3 and 4). Owner: web-api-engineer.
#ifndef WEB_SOCKETS_H
#define WEB_SOCKETS_H
    #include <Arduino.h>
    #include <WiFi.h>
    #include <AsyncTCP.h>
    #include <ESPAsyncWebServer.h>
    #include <support_method.h>
    #include <web_server.h>
    #include <ArduinoJson.h>

    extern AsyncWebSocket webSocket;
    extern AsyncEventSource rawEvents;
    extern volatile uint32_t ws_client_count;   // connected WS clients

    // Creates the RX worker and the outbound sender task and registers /ws and
    // /rawEvents (behind the auth/Origin gate). Called once from define_api.
    void initWebSocket();

    // Connected /rawEvents clients. Cheap, safe from any task (use this instead of
    // touching rawEvents directly, e.g. before producing DEBUG events).
    uint32_t events_client_count();

    // Enqueue a copy for the sender task. Safe from any task (not from an ISR).
    // Dropped when nobody is connected or the queue stays full for 20 ms.
    void send_to_ws(const String& message);
    void send_to_events(const String& data, const String& topic);

    void onRawEvents(AsyncEventSourceClient *client);
    void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
                 void *arg, uint8_t *data, size_t len);
#endif
