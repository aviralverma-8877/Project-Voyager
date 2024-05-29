#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include <Arduino.h>
#include <oled_display.h>
#include <tasks.h>
#include <wifi_support.h>
#include <lora_support.h>
#include <support_method.h>

// put function declarations here:

void setup() {
  Serial.begin(BAUD);
  // put your setup code here, to run once:
  config_gpios();
  init_oled();

  config_ap();
  config_lora();
  LoRa_sendMessage("Test MSG");
  serial_print("config done");
}

void loop() {
  // put your main code here, to run repeatedly:
}
