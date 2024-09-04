#include <web_server.h>

bool web_server_setup_done = false;
AsyncWebServer server(80);
QueueHandle_t send_packets;
QueueHandle_t recv_packets;
QueueHandle_t debug_msg;

void define_api(void *param)
{
  server.serveStatic("/", SPIFFS, "/");
  server.on("/send_akn", HTTP_POST, [](AsyncWebServerRequest *request)
  {
    serial_print("send_akn");
    AsyncWebParameter * j = request->getParam(0);
    String data = j->value();
    JsonDocument doc;
    deserializeJson(doc, data);
    uint8_t akn = doc["akn"];
    LoRa_sendAkn(akn);
    doc.clear();
    request->send(200);
  });
  server.on("/hostname", HTTP_GET, [](AsyncWebServerRequest *request)
  {
    serial_print("hostname");
    request->send(200, "text/plain", hostname);
  });
  server.on("/username", HTTP_GET, [](AsyncWebServerRequest *request)
  {
    serial_print("username");
    request->send(200, "text/plain", username);
  });
  server.on("/send_raw", HTTP_POST, [](AsyncWebServerRequest *request)
  {
    AsyncWebParameter * j = request->getParam(0);
    String data = j->value();
    QueueParam *packet = new QueueParam();
    packet->message = data;
    packet->type = RAW_DATA;
    packet->request = request;
    xQueueSend(send_packets, (void*)&packet, (TickType_t)2);
  });
  server.on("/lora_transmit", HTTP_POST, [](AsyncWebServerRequest *request)
  {
    AsyncWebParameter * j = request->getParam(0);
    String data = j->value();
    JsonDocument doc;
    doc["name"] = username;
    doc["mac"] = WiFi.macAddress();
    doc["data"] = data;
    String lora_payload;
    serializeJson(doc, lora_payload);
    doc.clear();
    QueueParam* taskParams = new QueueParam();
    taskParams->message=lora_payload;
    taskParams->type = LORA_MSG;
    taskParams->request = request;
    xQueueSend(send_packets, (void*)&taskParams, (TickType_t)2);
  });
  server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request)
  {
    serial_print("/config/lora_serial.json");
    request->send(200, "Restarting device ....");
    xTaskCreatePinnedToCore(restart, "restart", 6000, NULL, 1, NULL,1);
  });
  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request)
  {
    serial_print("/config/lora_serial.json");
    request->send(200, "Resetting device ....");
    xTaskCreatePinnedToCore(reset_device, "reset_devide", 6000, NULL, 1, NULL,1);
  });
  firmware_web_updater();                //Feature has to be disabled for including BLE features
  server.onNotFound([](AsyncWebServerRequest *request)
  {
    if (SPIFFS.exists("/index.html"))
    {
      request->send(SPIFFS, "/index.html");
    }
    else{
      serial_print("Redirected to /update");
      request->redirect("/update");
    }
   });
  initWebSocket();
  server.begin();
  vTaskDelete(NULL);
}

void firmware_web_updater()
{
  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "text/html", "<script>\
    function httpGet(action, options = {})\
    {\
        if(action == \"device\")\
        {\
            theUrl = '/control?device='+options;\
        }\
        try\
        {\
            var xmlHttp = new XMLHttpRequest();\
            xmlHttp.open( \"GET\", theUrl, false );\
            xmlHttp.send( null );\
            return xmlHttp.responseText;\
        }\
        catch(err){\
            return JSON.stringify({\"done\":false,\"error\":err});\
        }\
    }\
    </script>\
    <fieldset class=\"lora_transmission\">\
    <form method='POST' action='/update_flash' enctype='multipart/form-data'>\
      <div class='form-group mx-sm-3 mb-2'>\
        <label class='sr-only' for='firmare_file'><h5>firmware.bin</h5></label>\
        <input id='firmare_file' type='file' class='form-control row' placeholder='firmware.bin' name='update'>\
      </div>\
      <input class='btn btn-primary mb-2' type='submit' value='Update'>\
    </form><hr />\
    <form method='POST' action='/update_spiffs' enctype='multipart/form-data'>\
      <div class='form-group mx-sm-3 mb-2'>\
        <label class='sr-only' for='spiff_file'><h5>spiffs.bin</h5></label>\
        <input id='spiff_file' type='file' class='form-control row' placeholder='spiffs.bin' name='update'>\
      </div>\
      <input class='btn btn-primary mb-2' type='submit' value='Update'>\
    </form></fieldset>"); });

  server.on("/update_flash", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    bool shouldReboot = !Update.hasError();
    if(shouldReboot)
    {
      request->send_P(200, "text/html", "Upload successfull, Rebooting....<br /><a href='/'>Home Page</a>\
      <script>\
        setTimeout(\
          function()\
            {\
              window.location.href = \"/\"\
            },10000);\
      </script>");
    } }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
            {
    LoRa_txMode();
    if(!index){
      if(DEBUG)
        Serial.printf("Update Start: %s\n", filename.c_str());
      if(!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)){
        Update.printError(Serial);
      }
    }
    if(!Update.hasError()){
      if(Update.write(data, len) != len){
      if(DEBUG)
        Update.printError(Serial);
      }
    }
    if(final){
      if(Update.end(true)){
        if(DEBUG)
          Serial.printf("Update Success: %uB\n", index+len);
        xTaskCreatePinnedToCore(restart, "Restart", 6000, NULL, 1, NULL,1);
      } else {
        if(DEBUG)
          Update.printError(Serial);
      }
    } });

  server.on("/update_spiffs", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    bool shouldReboot = !Update.hasError();
    if(shouldReboot)
    {
      request->send_P(200, "text/html", "Upload successfull, Rebooting....<br /><a href='/'>Home Page</a>\
      <script>\
        setTimeout(\
          function()\
            {\
              window.location.href = \"/\"\
            },10000);\
      </script>");
    } }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
            {
    LoRa_txMode();
    if(!index){
      if(DEBUG)
        Serial.printf("Update Start: %s\n", filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)){
        Update.printError(Serial);
      }
    }
    if(!Update.hasError()){
      if(Update.write(data, len) != len){
      if(DEBUG)
        Update.printError(Serial);
      }
    }
    if(final){
      if(Update.end(true)){
        if(DEBUG)
          Serial.printf("Update Success: %uB\n", index+len);
        if(wifi_backup.backup_done)
        {
          serial_print("WiFi Setting backed up.");
          save_wifi_settings(wifi_backup.backup_config);
        }
      } else {
        if(DEBUG)
          Update.printError(Serial);
      }
    } });
}