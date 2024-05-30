#include <web_sockets.h>

AsyncWebSocket webSocket("/ws");

void initWebSocket() {
  serial_print("Initilizing Websockets");
  webSocket.onEvent(onEvent);
  server.addHandler(&webSocket);
}

void handle_ws_request(void *arg, uint8_t *data, size_t len)
{
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len) {
    switch (type) {
      case WS_EVT_CONNECT:
        serial_print("WebSocket client Connected.");
        break;
      case WS_EVT_DISCONNECT:
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