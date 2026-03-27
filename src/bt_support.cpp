#include "bt_support.h"
#include "lora_support.h"
#include "support_method.h"
#include "oled_display.h"
#include <ArduinoJson.h>

BluetoothSerial bt_serial;
volatile bool bt_client_connected = false;

static void bt_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
    if (event == ESP_SPP_SRV_OPEN_EVT) {
        bt_client_connected = true;
        set_bt_status(BT_CONNECTED);
        update_display();
        serial_print("BT client connected");
        bt_send("STATUS|Connected to " + String(BT_DEVICE_NAME));
    } else if (event == ESP_SPP_CLOSE_EVT) {
        bt_client_connected = false;
        set_bt_status(BT_DISCONNECTED);
        update_display();
        serial_print("BT client disconnected");
    }
}

void bt_init() {
    serial_print("Initializing Bluetooth");
    if (!bt_serial.begin(BT_DEVICE_NAME)) {
        serial_print("ERROR: Bluetooth init failed");
        return;
    }
    bt_serial.register_callback(bt_callback);
    set_bt_status(BT_DISCONNECTED);
    update_display();
    serial_print("Bluetooth ready: " + String(BT_DEVICE_NAME));
}

void bt_send(String data) {
    if (bt_client_connected) {
        bt_serial.println(data);
    }
}

void bt_task(void* param) {
    String line = "";
    serial_print("BT task started");

    while (true) {
        while (bt_serial.available()) {
            char c = (char)bt_serial.read();
            if (c == '\n') {
                line.trim();
                if (line.length() > 0) {
                    int sep = line.indexOf('|');
                    String prefix = (sep >= 0) ? line.substring(0, sep) : line;
                    String data   = (sep >= 0) ? line.substring(sep + 1) : "";
                    prefix.trim();
                    data.trim();

                    if (prefix == "SEND") {
                        // Build LoRa message JSON: {"name": ..., "mac": ..., "data": ...}
                        JsonDocument doc;
                        doc["name"] = username;
                        uint64_t chipid = ESP.getEfuseMac();
                        char mac_str[18];
                        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                            (uint8_t)(chipid >> 40), (uint8_t)(chipid >> 32),
                            (uint8_t)(chipid >> 24), (uint8_t)(chipid >> 16),
                            (uint8_t)(chipid >> 8),  (uint8_t)(chipid));
                        doc["mac"] = mac_str;
                        doc["data"] = data;
                        String payload;
                        serializeJson(doc, payload);
                        doc.clear();

                        QueueParam* pkt = new QueueParam();
                        if (pkt) {
                            pkt->message = payload;
                            pkt->type = LORA_MSG;
                            pkt->send_response = true;
                            if (xQueueSend(send_packets, (void*)&pkt, (TickType_t)2) != pdTRUE) {
                                delete pkt;
                                bt_send("TX|FAIL");
                            }
                        }

                    } else if (prefix == "RAW") {
                        QueueParam* pkt = new QueueParam();
                        if (pkt) {
                            pkt->message = data;
                            pkt->type = RAW_DATA;
                            pkt->send_response = true;
                            if (xQueueSend(send_packets, (void*)&pkt, (TickType_t)2) != pdTRUE) {
                                delete pkt;
                                bt_send("TX|FAIL");
                            }
                        }

                    } else if (prefix == "AKN") {
                        uint8_t akn = (uint8_t)data.toInt();
                        LoRa_sendAkn(akn);

                    } else if (prefix == "SERIAL") {
                        DebugQueueParam* p = new DebugQueueParam();
                        if (p) {
                            p->message = data;
                            if (xQueueSend(serial_packet_send, (void*)&p, (TickType_t)2) != pdTRUE)
                                delete p;
                        }
                    }
                }
                line = "";
            } else if (c != '\r') {
                line += c;
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
