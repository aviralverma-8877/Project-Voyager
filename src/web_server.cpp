#include <web_server.h>

AsyncWebServer server(80);
QueueHandle_t send_packets;
QueueHandle_t recv_packets;

void define_api()
{
  send_packets = xQueueCreate(20, sizeof(QueueParam*));
  recv_packets = xQueueCreate(20, sizeof(QueueParam*));
  server.serveStatic("/", SPIFFS, "/");
  server.on("/hostname", HTTP_GET, [](AsyncWebServerRequest *request)
  {
    serial_print("hostname");
    request->send(200, "text/plain", hostname);
  });
  server.on("/send_raw", HTTP_POST, [](AsyncWebServerRequest *request)
  {
    AsyncWebParameter * j = request->getParam(0);
    String data = j->value();
    QueueParam *packet = new QueueParam();
    packet->message = data;
    packet->type = RAW_DATA;
    xQueueSend(send_packets, (void*)&packet, (TickType_t)2);
    request->send(200);
  });
  server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request)
  {
    serial_print("/config/lora_serial.json");
    request->send(200, "Restarting device ....");
    xTaskCreate(restart, "restart", 6000, NULL, 1, NULL);
  });
  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request)
  {
    serial_print("/config/lora_serial.json");
    request->send(200, "Resetting device ....");
    xTaskCreate(reset_device, "reset_devide", 6000, NULL, 1, NULL);
  });
  firmware_web_updater();
  server.onNotFound([](AsyncWebServerRequest *request)
  {
    if (SPIFFS.exists("/index.html"))
    {
      serial_print("Redirected to /index.html");
      request->redirect("/index.html");
    }
    else{
      serial_print("Redirected to /update");
      request->redirect("/update");
    }
   });
  server.begin();
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
    </form>"); });

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
        xTaskCreate(restart, "Restart", 6000, NULL, 1, NULL);
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