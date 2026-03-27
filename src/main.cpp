#include <Arduino.h>
#include <oled_display.h>
#include <tasks.h>
#include <lora_support.h>
#include <support_method.h>
#include <bt_support.h>
#include "FS.h"
#include "LittleFS.h"

// Queue handles (defined here, declared extern in lora_support.h)
QueueHandle_t send_packets;
QueueHandle_t recv_packets;
QueueHandle_t debug_msg;
QueueHandle_t serial_packet_send;
QueueHandle_t serial_packet_rec;

void setup() {
  Serial.begin(BAUD);
  send_packets = xQueueCreate(20, sizeof(QueueParam*));
  recv_packets = xQueueCreate(20, sizeof(QueueParam*));
  debug_msg = xQueueCreate(20, sizeof(DebugQueueParam*));
  serial_packet_send = xQueueCreate(20, sizeof(DebugQueueParam*));
  serial_packet_rec = xQueueCreate(20, sizeof(DebugQueueParam*));
  xTaskCreatePinnedToCore(debugger_print, "debugger_print", 6000, NULL, 1, &debug_handler, 1);
  if (!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED))
  {
    serial_print("LittleFS Mount Failed");
    return;
  }
  get_lora_serial();
  get_username();
  config_gpios();
  init_oled();
  bt_init();
  config_lora();
  setupTasks();
  xTaskCreatePinnedToCore(bt_task, "bt_task", 4000, NULL, 2, NULL, 1);
  serial_print("config done");
}

void loop() {
  // All work is done in FreeRTOS tasks
}
