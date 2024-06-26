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
  if(DEBUG)
    Serial.begin(BAUD);
  if (!SPIFFS.begin(FORMAT_SPIFFS_IF_FAILED))
  {
    serial_print("SPIFFS Mount Failed");
    return;
  }
  get_username();
  config_gpios();
  init_oled();

  // WiFi.onEvent(onWifiConnect, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  // WiFi.onEvent(onWifiDisconnect, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent(onWifiConnect, SYSTEM_EVENT_STA_GOT_IP);
  WiFi.onEvent(onWifiDisconnect, SYSTEM_EVENT_STA_DISCONNECTED);
  config_wifi();

  define_api();
  initWebSocket();
  
  config_lora();
  setupTickers();
  serial_print("config done");
}

void loop() {
  // put your main code here, to run repeatedly:
}
