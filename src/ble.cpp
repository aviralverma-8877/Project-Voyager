#include <ble.h>

void ble_setup()
{
    serial_print("BLE setup");
    BLEDevice::init(PROJECT_NAME);
    BLEServer *pServer = BLEDevice::createServer();
    BLEService *pService = pServer->createService(SERVICE_HANDLER_UUID);
    BLECharacteristic *pCharacteristic = pService->createCharacteristic(
                                     CHARACTERISTIC_HANDLER_UUID,
                                     BLECharacteristic::PROPERTY_READ |
                                     BLECharacteristic::PROPERTY_WRITE
                                     );
    pCharacteristic->setValue("Hello from ESP32");
    BLEAdvertising *pAdvertising = pServer->getAdvertising();
    pAdvertising->start();
}