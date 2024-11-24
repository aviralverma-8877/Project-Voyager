#include <ble.h>

BLECharacteristic *pTxCharacteristic;
BLECharacteristic *pRxCharacteristic;

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer) {
    BLEDevice::startAdvertising();
  };

  void onDisconnect(BLEServer *pServer) {
  }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String rxValue = pCharacteristic->getValue().c_str();

    if (rxValue.length() > 0) {
      JsonDocument doc;
      deserializeJson(doc, rxValue);
      handle_operations(doc);
    }
  }
  void onRead(){

  }
};

void ble_setup()
{
    serial_print("BLE setup");
    BLEDevice::init(PROJECT_NAME);
    BLEServer *pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService *pService = pServer->createService(SERVICE_HANDLER_UUID);
//Setting Tx Charecteristic
    BLECharacteristic *pTxCharacteristic = pService->createCharacteristic(
                                        TX_CHARACTERISTIC_HANDLER_UUID,
                                        BLECharacteristic::PROPERTY_NOTIFY
                                    );
    pTxCharacteristic->addDescriptor(new BLE2902());
//Setting Rx Charecteristic
    BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
                                        RX_CHARACTERISTIC_HANDLER_UUID,
                                        BLECharacteristic::PROPERTY_WRITE
                                    );
    pRxCharacteristic->setCallbacks(new MyCallbacks());
    BLEAdvertising *pAdvertising = pServer->getAdvertising();
    pAdvertising->start();
}

void send_to_ble(String msg)
{
    pTxCharacteristic->setValue(msg.c_str());
    pTxCharacteristic->notify();
    vTaskDelay(10/portTICK_PERIOD_MS);
}