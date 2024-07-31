#include <web_sockets.h>

bool ws_connected = false;
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
  String json = "";
  for(int i=0; i<len; i++)
  {
    char r = (char)data[i];
    json += r;
  }
  serial_print(json);
  JsonDocument doc;
  deserializeJson(doc, json);
  doc.shrinkToFit();
  handle_operations(doc);
  doc.clear();
}

void send_to_events(void* param)
{
  EventParam* params = (EventParam*)param;
  String return_value = (String)params->data;
  free(params);
  String topic = (String)params->topic;
  rawEvents.send(return_value.c_str(), topic.c_str());
  vTaskDelete(NULL);
}

void send_to_ws(void* param)
{
  TaskParameters* params = (TaskParameters*)param;
  String return_value = (String)params->data;
  free(params);
  if(ws_connected)
  {
    webSocket.textAll(return_value);
  }
  vTaskDelete(NULL);
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