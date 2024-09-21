#ifndef ble
    #define ble
    #include <BLEDevice.h>
    #include <BLEUtils.h>
    #include <BLEServer.h>
    #include <support_method.h>
    //SERVICE UUIDs
    #define SERVICE_HANDLER_UUID        "47589dc1-833d-4c52-9d3f-97074ccdb6ff"
    #define CHARACTERISTIC_HANDLER_UUID "ec2dcc5b-e09c-4faf-af3b-e9c2d78e649d"
    void ble_setup();
#endif
