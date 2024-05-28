#include <Arduino.h>
#include <oled_display.h>
#include <tasks.h>

// put function declarations here:

void setup() {
  Serial.begin(BAUD);
  // put your setup code here, to run once:
  init_oled();
  pinMode(LED, OUTPUT);
  pinMode(BTN1, INPUT_PULLDOWN);
  pinMode(BTN2, INPUT_PULLDOWN);
  digitalWrite(LED, HIGH);
  xTaskCreatePinnedToCore(led_nortifier, "LED_Nortifier", 4096, NULL, 3, NULL, ARDUINO_RUNNING_CORE);
  xTaskCreatePinnedToCore(btn_1_intrupt, "BTN_1_Nortifier", 4096, NULL, 3, NULL, ARDUINO_RUNNING_CORE);
  xTaskCreatePinnedToCore(btn_2_intrupt, "BTN_2_Nortifier", 4096, NULL, 3, NULL, ARDUINO_RUNNING_CORE);
}

void loop() {
  // put your main code here, to run repeatedly:
}
