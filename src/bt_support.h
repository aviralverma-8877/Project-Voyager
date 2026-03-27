#ifndef bt_support_h
#define bt_support_h

#include <Arduino.h>
#include <BluetoothSerial.h>

#define BT_DEVICE_NAME PROJECT_NAME

extern BluetoothSerial bt_serial;
extern volatile bool bt_client_connected;

void bt_init();
void bt_send(String data);
void bt_task(void* param);

#endif
