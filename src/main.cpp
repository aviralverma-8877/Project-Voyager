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
  pinMode(LED, OUTPUT);
  pinMode(BTN1, INPUT_PULLDOWN);
  pinMode(BTN2, INPUT_PULLDOWN);
  digitalWrite(LED, HIGH);

  init_oled();

  config_ap();
  config_lora();
  LoRa_sendMessage("Test MSG");

  xTaskCreate(led_nortifier, "LED_Nortifier", 6000, NULL, 0, NULL);
  xTaskCreate(btn_intrupt, "BTN_Nortifier", 6000, NULL, 0, NULL);
  serial_print("config done");
}

void loop() {
  // put your main code here, to run repeatedly:
}
