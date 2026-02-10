#include <web_sockets.h>

volatile uint32_t ws_client_count = 0;
AsyncWebSocket webSocket("/ws");
AsyncEventSource rawEvents("/rawEvents");

void initWebSocket() {
  serial_print("Initilizing Websockets");
  webSocket.onEvent(onEvent);
  rawEvents.onConnect(onRawEvents);
  server.addHandler(&rawEvents);
  server.addHandler(&webSocket);
}

void clean_up_client(void* params)
{
  while(true)
  {
    webSocket.cleanupClients();
    vTaskDelay(50/portTICK_PERIOD_MS);
  }
}

void handle_ws_request(void *arg, uint8_t *data, size_t len)
{
  String json;
  json.reserve(len + 1);
  for(int i=0; i<len; i++)
  {
    json += (char)data[i];
  }
  serial_print(json);
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);
  if(error) {
    serial_print("ERROR: WebSocket JSON parsing failed: "+(String)error.c_str());
    return;
  }
  doc.shrinkToFit();
  handle_operations(doc);
  doc.clear();
}

void send_to_events(String data, String topic)
{
  rawEvents.send(data.c_str(), topic.c_str());
}

void send_to_ws(String return_value)
{
  if(ws_client_count > 0)
  {
    webSocket.textAll(return_value);
  }
}

void onRawEvents(AsyncEventSourceClient *client){
  if(client->lastId()){
    serial_print("Client reconnected! Last message ID that it got is: "+client->lastId());
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len) {
    switch (type) {
      case WS_EVT_CONNECT:
        ws_client_count++;
        serial_print("WebSocket client #"+(String)client->id()+" connected. Total clients: "+(String)ws_client_count);
        break;
      case WS_EVT_DISCONNECT:
        if(ws_client_count > 0) {
          ws_client_count--;
        }
        serial_print("WebSocket client #"+(String)client->id()+" disconnected. Total clients: "+(String)ws_client_count);
        break;
      case WS_EVT_DATA:
        serial_print("WebSocket data received from client #"+(String)client->id());
        handle_ws_request(arg, data, len);
        break;
      case WS_EVT_PONG:
        break;
      case WS_EVT_ERROR:
        serial_print("WebSocket error from client #"+(String)client->id());
        break;
  }
}