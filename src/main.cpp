#include <Arduino.h>
#include <oled_display.h>
#include <tasks.h>
#include <wifi_support.h>
#include <lora_support.h>
#include <support_method.h>
#include <web_server.h>
#include <web_sockets.h>
#include "FS.h"
#include "SPIFFS.h"
// put function declarations here:

void setup() {
  Serial.begin(BAUD);
  if (!SPIFFS.begin(FORMAT_SPIFFS_IF_FAILED))
  {
    serial_print("SPIFFS Mount Failed");
    return;
  }
  config_gpios();
  init_oled();

  config_wifi();
  // WiFi.onEvent(onWifiConnect, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  // WiFi.onEvent(onWifiDisconnect, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent(onWifiConnect, SYSTEM_EVENT_STA_GOT_IP);
  WiFi.onEvent(onWifiDisconnect, SYSTEM_EVENT_STA_DISCONNECTED);

  define_api();
  initWebSocket();
  
  config_lora();
  serial_print("config done");
}

void loop() {
  // put your main code here, to run repeatedly:
}
