#include <Arduino.h>
#include <oled_display.h>
#include <tasks.h>
#include <lora_support.h>
#include <support_method.h>
#include "FS.h"
#include "SPIFFS.h"
// put function declarations here:

void setup() {
  Serial.begin(BAUD);
  send_packets = xQueueCreate(20, sizeof(QueueParam*));
  recv_packets = xQueueCreate(20, sizeof(QueueParam*));
  debug_msg = xQueueCreate(20, sizeof(DebugQueueParam*));
  xTaskCreatePinnedToCore(debugger_print, "debugger_print", 6000, NULL, 1, &debug_handler, 1);
  if (!SPIFFS.begin(FORMAT_SPIFFS_IF_FAILED))
  {
    serial_print("SPIFFS Mount Failed");
    return;
  }
  get_lora_serial();
  get_username();
  config_gpios();
  init_oled();

  config_lora();
  setupTasks();
  serial_print("config done");
}

void loop() {
  // put your main code here, to run repeatedly:
}
