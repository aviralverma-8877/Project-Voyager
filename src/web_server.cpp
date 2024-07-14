#include <web_server.h>

AsyncWebServer server(80);

void define_api()
{
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
    {
      if (SPIFFS.exists("/index.html"))
      {
        File index = SPIFFS.open("/index.html");
        if (index) {
            serial_print("index");
            request->send(SPIFFS, "/index.html", "text/html");
        }
        else{
          serial_print("Redirected to /update");
          request->redirect("/update");
        }
      }
      else{
        serial_print("Redirected to /update");
        request->redirect("/update");
      }
    });
  server.onNotFound([](AsyncWebServerRequest *request){
    serial_print("Not Found");
    request->redirect("/");
  });
  server.on("/bootstrap.min.css", HTTP_GET, [](AsyncWebServerRequest *request)
            {
        serial_print("bootstrap.min.css");
        request->send(SPIFFS, "/bootstrap.min.css", "text/css"); });
  server.on("/jquery-3.7.1.min.js", HTTP_GET, [](AsyncWebServerRequest *request)
            {
        serial_print("jquery-3.7.1.min.js");
        request->send(SPIFFS, "/jquery-3.7.1.min.js", "text/javascript"); });
  server.on("/bootstrap.bundle.min.js", HTTP_GET, [](AsyncWebServerRequest *request)
            {
        serial_print("bootstrap.bundle.min.js");
        request->send(SPIFFS, "/bootstrap.bundle.min.js", "text/javascript"); });
  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request)
            {
        serial_print("script.js");
        request->send(SPIFFS, "/script.js", "text/javascript"); });
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request)
            {
        serial_print("favicon.ico");
        request->send(SPIFFS, "/favicon.ico", "image/vnd"); });
  server.on("/dashboard.html", HTTP_GET, [](AsyncWebServerRequest *request)
            {
        serial_print("dashboard.html");
        request->send(SPIFFS, "/dashboard.html", "text/html"); });
  server.on("/wifi.html", HTTP_GET, [](AsyncWebServerRequest *request)
            {
        serial_print("wifi.html");
        request->send(SPIFFS, "/wifi.html", "text/html"); });
  server.on("/lora.html", HTTP_GET, [](AsyncWebServerRequest *request)
            {
        serial_print("lora.html");
        request->send(SPIFFS, "/lora.html", "text/html"); });
  server.on("/file_transfer.html", HTTP_GET, [](AsyncWebServerRequest *request)
            {
        serial_print("file_transfer.html");
        request->send(SPIFFS, "/file_transfer.html", "text/html"); });
  server.on("/Voyager_spacecraft.jpg", HTTP_GET, [](AsyncWebServerRequest *request)
            {
        serial_print("Voyager_spacecraft.jpg");
        request->send(SPIFFS, "/Voyager_spacecraft.jpg", "image/jpeg"); });
  server.on("/hostname", HTTP_GET, [](AsyncWebServerRequest *request)
            {
        serial_print("hostname");
        request->send(200, "text/plain", hostname); });
  server.on("/lora_config.json", HTTP_GET, [](AsyncWebServerRequest *request)
            {
          serial_print("/config/lora_config.json");
          request->send(SPIFFS, "/config/lora_config.json", "text/json"); });
  server.on("/lora_serial.json", HTTP_GET, [](AsyncWebServerRequest *request)
            {
          serial_print("/config/lora_serial.json");
          request->send(SPIFFS, "/config/lora_serial.json", "text/json"); });
  server.on("/send_raw", HTTP_POST, [](AsyncWebServerRequest * request) 
          {
            AsyncWebParameter * j = request->getParam(0);
            String data = j->value();
            serial_print(data);
            packets.push(&data);
            TaskHandle_t xHandle = NULL;
            xTaskCreate(LoRa_sendRaw, "LoRa_sendRaw", 20000, NULL, 2, &xHandle);
            eTaskState ts = eTaskGetState(xHandle);
            while(ts == eRunning)
            {
              ts = eTaskGetState(xHandle);
            }
            request->send(200);
          });
  if(DEBUG)
  {
    server.on("/config.json", HTTP_GET, [](AsyncWebServerRequest *request)
              {
          serial_print("/config/wifi_config.json");
          request->send(SPIFFS, "/config/wifi_config.json", "text/json"); });
    server.on("/mqtt_config.json", HTTP_GET, [](AsyncWebServerRequest *request)
              {
          serial_print("/config/mqtt_config.json");
          request->send(SPIFFS, "/config/mqtt_config.json", "text/json"); });
  }
  firmware_web_updater();
  server.begin();
}

void firmware_web_updater()
{
  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", "<script>\
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
    </form>");
  });

  server.on("/update_flash", HTTP_POST, [](AsyncWebServerRequest *request){
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
    }
  },[](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
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
    }
  });

  server.on("/update_spiffs", HTTP_POST, [](AsyncWebServerRequest *request){
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
    }
  },[](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
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
    }
  });
}