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
  get_lora_serial();
  get_username();
  config_gpios();
  init_oled();
  send_packets = xQueueCreate(20, sizeof(QueueParam*));
  recv_packets = xQueueCreate(20, sizeof(QueueParam*));
  debug_msg = xQueueCreate(20, sizeof(DebugQueueParam*));
  // WiFi.onEvent(onWifiConnect, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  // WiFi.onEvent(onWifiDisconnect, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent(onWifiConnect, SYSTEM_EVENT_STA_GOT_IP);
  WiFi.onEvent(onWifiDisconnect, SYSTEM_EVENT_STA_DISCONNECTED);
  xTaskCreatePinnedToCore(config_wifi, "config_wifi", 6000, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(define_api, "define_api", 6000, NULL, 2, NULL, 1);

  config_lora();
  setupTasks();
  serial_print("config done");
}

void loop() {
  // put your main code here, to run repeatedly:
}
