#include <Arduino.h>
#include <oled_display.h>

#define builtin_led 2
// put function declarations here:

void setup() {
  // put your setup code here, to run once:
  init_oled();
  pinMode(builtin_led, OUTPUT);
  digitalWrite(builtin_led, HIGH);
}

void loop() {
  // put your main code here, to run repeatedly:
  digitalWrite(builtin_led, LOW);
  delay(500);
  digitalWrite(builtin_led, HIGH);
  delay(500);
}
