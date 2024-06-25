#include<mqtt_support.h>

Ticker mqttReconnectTimer;
String mqtt_topic_to_subscribe = "";
String mqtt_topic_to_send_raw = "";
String mqtt_topic_to_publish = "";
AsyncMqttClient mqttClient;

void setup_mqtt()
{
    serial_print("Setting up mqtt");
    while(!WiFi.isConnected()){}
    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    mqttClient.onSubscribe(onMqttSubscribe);
    mqttClient.onUnsubscribe(onMqttUnsubscribe);
    mqttClient.onMessage(onMqttMessage);
    mqttClient.onPublish(onMqttPublish);
    String mqtt_config = get_mqtt_config();
    JsonDocument doc;
    deserializeJson(doc, mqtt_config);
    const char* host_string = doc["host"];
    uint16_t port = doc["port"];
    bool auth = doc["auth"];
    const char* uname = doc["username"];
    const char* pass = doc["password"];

    String raw_data = doc["raw_data"];
    mqtt_topic_to_send_raw = raw_data;

    String pub_topic = doc["pub_topic"];
    mqtt_topic_to_publish = pub_topic;

    String sub_topic = doc["sub_topic"];
    mqtt_topic_to_subscribe = sub_topic;

    IPAddress host;
    host.fromString(host_string);
    serial_print(host.toString());
    serial_print(String(port));

    if(auth)
    {
        serial_print(uname);
        serial_print(pass);
        mqttClient.setCredentials(uname, pass);
    }
    mqttClient.setServer(host, port);
    connectToMqtt();
}

void connectToMqtt()
{
    serial_print("Connecting to MQTT");
    mqttClient.connect();
}

void onMqttConnect(bool sessionPresent) {
    serial_print("MQTT connected.");
    // display_buffer[4].msg = "MQTT";
    // display_text_oled();
    String mac = WiFi.macAddress();
    String sub_topic = "voyager/"+mac+"/"+mqtt_topic_to_subscribe;
    String raw_data = "voyager/"+mac+"/"+mqtt_topic_to_send_raw;
    mqttClient.subscribe(sub_topic.c_str(), 2);
    mqttClient.subscribe(raw_data.c_str(), 2);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
    serial_print("MQTT is disconnected.");
    switch (reason)
    {
        case AsyncMqttClientDisconnectReason::TCP_DISCONNECTED:
            serial_print("TCP_DISCONNECTED");
        break;
        case AsyncMqttClientDisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION:
            serial_print("MQTT_UNACCEPTABLE_PROTOCOL_VERSION");
        break;
        case AsyncMqttClientDisconnectReason::MQTT_IDENTIFIER_REJECTED:
            serial_print("MQTT_IDENTIFIER_REJECTED");
        break;
        case AsyncMqttClientDisconnectReason::MQTT_SERVER_UNAVAILABLE:
            serial_print("MQTT_SERVER_UNAVAILABLE");
        break;
        case AsyncMqttClientDisconnectReason::MQTT_MALFORMED_CREDENTIALS:
            serial_print("MQTT_MALFORMED_CREDENTIALS");
        break;
        case AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED:
            serial_print("MQTT_NOT_AUTHORIZED");
        break;
        case AsyncMqttClientDisconnectReason::ESP8266_NOT_ENOUGH_SPACE:
            serial_print("ESP8266_NOT_ENOUGH_SPACE");
        break;
        case AsyncMqttClientDisconnectReason::TLS_BAD_FINGERPRINT:
            serial_print("TLS_BAD_FINGERPRINT");
        break;
        default:
        break;
    }
    if (WiFi.isConnected()) {
        mqttReconnectTimer.once(2, connectToMqtt);
    }
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos) {
    serial_print("Subscribed to MQTT topic.");
}

void onMqttUnsubscribe(uint16_t packetId) {
    serial_print("unsubscribed from MQTT topic.");
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
    serial_print("MQTT message recieved.");
    String msg = "";
    for(int i=index; (unsigned)i<len;i++)
    {
        msg += payload[i];
    }
    serial_print(msg);
    String mac = WiFi.macAddress();
    String sub_topic = "voyager/"+mac+"/"+mqtt_topic_to_subscribe;
    String raw_data = "voyager/"+mac+"/"+mqtt_topic_to_send_raw;
    if(strcmp(topic, raw_data.c_str()) == 0)
    {
        LoRa_sendRaw(msg);
    }
    else if(strcmp(topic, sub_topic.c_str()) == 0)
    {
        LoRa_sendMessage(msg);
    }
}

void onMqttPublish(uint16_t packetId) {
    serial_print("MQTT message published.");
}

void send_to_mqtt(String msg)
{
    String mac = WiFi.macAddress();
    String topic = "voyager/"+mac+"/"+mqtt_topic_to_publish;
    serial_print("Sending msg to mqtt");
    serial_print(topic);
    serial_print(msg);
    mqttClient.publish(topic.c_str(), 2, false, msg.c_str(), msg.length());
}

String get_mqtt_config()
{
    if (SPIFFS.exists("/config/mqtt_config.json"))
    {
        File file = SPIFFS.open("/config/mqtt_config.json");
        if(!file){
            return "";
        }
        String mqtt_config;
        while(file.available()){
            mqtt_config += file.readString();
        }
        file.close();
        serial_print("Reading mqtt settings");
        serial_print(mqtt_config);
        return mqtt_config;
    }
    return "";
}