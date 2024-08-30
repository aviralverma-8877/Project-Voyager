#ifndef ble
    #define ble
    #include <BLEDevice.h>
    #include <BLEUtils.h>
    #include <BLEServer.h>
    #include <support_method.h>
    // BLE Service
        // Web API
        #define SERVICE_SEND_AKN "53349b9d-b77f-46f8-8390-3a198006dafc"
        #define SERVICE_HOSTNAME "ccc02be1-42f1-4235-89d2-4f5eb0d17bed"
        #define SERVICE_GET_USERNAME "cb391fc1-2ebd-49e2-bb2a-a12af8195bd1"
        #define SERVICE_SEND_RAW "ea8af563-8cc8-4c2f-8107-de3e50ac9904"
        #define SERVICE_LORA_TRANSMIT "bd809a30-b7e4-4941-9f42-4ecccb1b1a38"
        #define SERVICE_RESTART "6151a11e-779c-48a1-9c31-68fde152c887"
        #define SERVICE_RESET "de044ecf-b9f9-4c5f-99ab-52070d8d2f03"

        // Socket API
        #define SERVICE_SET_USERNAME "65cc8ea1-4df3-40c2-8528-2f563a985e09"
        #define SERVICE_SET_LORA_CONFIG "4e3d4973-a814-446c-9ea2-10a0538c256f"
        #define SERVICE_SET_SERIAL_MODE "b092f7ad-1e44-473b-aeaf-e9fac9b07146"
        #define SERVICE_GET_SERIAL_MODE "cfd52cf0-d7cb-41de-9e84-8ae6d74ac1f0"

    // BLE CHARACTERISTIC
        // Web API
        #define CHARACTERISTIC_SEND_AKN "37890272-3cb7-43e2-8e0b-8d7b1f069eae"
        #define CHARACTERISTIC_HOSTNAME "daa07eec-f47e-4d93-b343-d4f2ee2d7738"
        #define CHARACTERISTIC_GET_USERNAME "0ca59f25-fcc8-4a70-9352-c30f0ef58987"
        #define CHARACTERISTIC_SEND_RAW "138b69df-6e91-4c99-9a00-5f7c60055607"
        #define CHARACTERISTIC_LORA_TRANSMIT "ad3e526a-ff54-4c82-ab65-ede37965f93b"
        #define CHARACTERISTIC_RESTART "135e1d3f-2b7b-4c53-8ecf-6e9db100c077"
        #define CHARACTERISTIC_RESET "ece25fe7-988e-447f-a2f7-4b336771a0c8"

        // Socket API
        #define CHARACTERISTIC_SET_USERNAME "45447c97-0fa1-4ded-9226-956c3b530204"
        #define CHARACTERISTIC_SET_LORA_CONFIG "4448173c-fdf1-4ae0-9a3b-9bdf0a98eed0"
        #define CHARACTERISTIC_SET_SERIAL_MODE "2491c6e2-ae12-4c01-b229-c39add6978f5"
        #define CHARACTERISTIC_GET_SERIAL_MODE "32c9c7c7-1e6a-4244-91c0-c7272b9ecd51"
    void init_ble();
#endif