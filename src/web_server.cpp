#include <web_server.h>

AsyncWebServer server(80);

void define_api()
{
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        serial_print("index.html");
        File index = SPIFFS.open("/index.html");
        if (index) {
            request->send(SPIFFS, "/index.html", "text/html");
        }
        else{
            request->send_P(200, "text/html", "<html>\
                <title>No SPIFFS found.</title>\
                <body><h2>No SPIFFS uploaded to ESP.<br />Please upload the SPIFFS binary.</h2></body>\
            </html>");
        }
    });
    server.on("/bootstrap.min.css", HTTP_GET, [](AsyncWebServerRequest *request){
        serial_print("bootstrap.min.css");
        request->send(SPIFFS, "/bootstrap.min.css", "text/html");
    });
    server.on("/jquery-3.7.1.min.js", HTTP_GET, [](AsyncWebServerRequest *request){
        serial_print("jquery-3.7.1.min.js");
        request->send(SPIFFS, "/jquery-3.7.1.min.js", "text/html");
    });
        server.on("/bootstrap.bundle.min.js", HTTP_GET, [](AsyncWebServerRequest *request){
        serial_print("bootstrap.bundle.min.js");
        request->send(SPIFFS, "/bootstrap.bundle.min.js", "text/html");
    });
    server.begin();
}