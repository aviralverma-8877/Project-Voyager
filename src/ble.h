#ifndef ble
    #define ble
    #include <BLEDevice.h>
    #include <BLEUtils.h>
    #include <BLEServer.h>
    #include <BLE2902.h>
    #include <support_method.h>
    //SERVICE UUIDs
    #define SERVICE_HANDLER_UUID        "47589dc1-833d-4c52-9d3f-97074ccdb6ff"
    #define TX_CHARACTERISTIC_HANDLER_UUID "39036e61-b3ff-4fbf-9f8e-5d63a6686d04"
    #define RX_CHARACTERISTIC_HANDLER_UUID "ec2dcc5b-e09c-4faf-af3b-e9c2d78e649d"
    extern BLECharacteristic *pTxCharacteristic;
    extern BLECharacteristic *pRxCharacteristic;
    void ble_setup();
    void send_to_ble(String msg);
#endif
