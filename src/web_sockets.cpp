#include <web_sockets.h>

bool ws_connected = false;
Ticker TickerForWSClientCleanup;
AsyncWebSocket webSocket("/ws");

void initWebSocket() {
  serial_print("Initilizing Websockets");
  webSocket.onEvent(onEvent);
  server.addHandler(&webSocket);
  TickerForWSClientCleanup.attach_ms(10, cleanup_client);
}

void cleanup_client()
{
  webSocket.cleanupClients();
}

void handle_ws_request(void *arg, uint8_t *data, size_t len)
{
  String json = "";
  for(int i=0; i<len; i++)
  {
    char r = (char)data[i];
    json += r;
  }
  serial_print(json);
  JsonDocument doc;
  deserializeJson(doc, json);
  handle_operations(doc);
}

void send_to_ws(String return_value)
{
  if(ws_connected)
  {
    serial_print(return_value);
    webSocket.textAll(return_value);
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len) {
    switch (type) {
      case WS_EVT_CONNECT:
        ws_connected = true;
        serial_print("WebSocket client Connected.");
        break;
      case WS_EVT_DISCONNECT:
        ws_connected = false;
        serial_print("WebSocket client Disconnected.");
        break;
      case WS_EVT_DATA:
        serial_print("WebSocket data Recieved.");
        handle_ws_request(arg, data, len);
        break;
      case WS_EVT_PONG:
        break;
      case WS_EVT_ERROR:
        break;
  }
}