#include <ble.h>

void init_ble()
{
    BLEDevice::init(username.c_str());
    BLEServer *pServer = BLEDevice::createServer();
    BLEService *pService = pServer->createService(SERVICE_SEND_AKN);
    BLECharacteristic *pCharacteristic =
    pService->createCharacteristic(CHARACTERISTIC_SEND_AKN, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);

    pCharacteristic->setValue("SEND_AKN");
    pService->start();

    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_SEND_AKN);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);  // functions that help with iPhone connections issue
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
    serial_print("Characteristic defined! Now you can read it in your phone!");
    display_buffer[1].msg = "BLE MODE";
    display_buffer[2].msg = username;
    display_text_oled();
}
