// Improv Serial WiFi setup over USB (see improv_serial.cpp).
#ifndef VOYAGER_IMPROV_SERIAL_H
#define VOYAGER_IMPROV_SERIAL_H

#include <Arduino.h>

// Feed one byte received on USB serial while the LoRa-serial bridge is off.
void improv_feed(uint8_t b);
// Drop a half-received packet (e.g. when the bridge turns on).
void improv_reset();

#endif
