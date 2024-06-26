#ifndef mqtt_support
    #define mqtt_support
    #include <Arduino.h>
    #include <WiFi.h>
    extern "C" {
        #include "freertos/FreeRTOS.h"
        #include "freertos/timers.h"
    }
    #include <AsyncMqttClient.h>
    #include "web_sockets.h"
    #include "lora_support.h"
    #include "SPIFFS.h"
    #include "support_method.h"

    extern Ticker mqttReconnectTimer;
    extern AsyncMqttClient mqttClient;
    extern String mqtt_topic_to_subscribe;
    extern String mqtt_topic_to_send_raw;
    extern String mqtt_topic_to_publish;

    void ping_mqtt(String msg);
    void connectToMqtt();
    void onMqttConnect(bool sessionPresent);
    void onMqttDisconnect(AsyncMqttClientDisconnectReason reason);
    void onMqttSubscribe(uint16_t packetId, uint8_t qos);
    void onMqttUnsubscribe(uint16_t packetId);
    void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total);
    void onMqttPublish(uint16_t packetId);
    void send_to_mqtt(String msg);
    String get_mqtt_config();
#endif