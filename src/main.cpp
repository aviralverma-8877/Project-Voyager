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

  config_ap();
  setup_dns();
  define_api();
  initWebSocket();
  
  config_lora();
  LoRa_sendMessage("Test MSG");
  serial_print("config done");
}

void loop() {
  // put your main code here, to run repeatedly:
}
