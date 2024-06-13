#include <support_method.h>

String hostname;
DNSServer dnsServer;
Ticker TickerForBtnPresses;
Ticker TickerForLoraBeacon;
Ticker TickerForLedNotification;
Ticker TickerForDNSRequest;

void handle_operations(JsonDocument doc)
{
    const char* request_type = doc["request-type"];
    serial_print(request_type);
    if(strcmp(request_type, "wifi_ssid_scan") == 0)
    {
        String json_string;
        xTaskCreate(scan_ssid, "scan_wifi", 6000, NULL, 2, NULL);
    }
    if(strcmp(request_type, "connect_wifi") == 0)
    {
        const char * wifi_ssid = doc["wifi_ssid"];
        serial_print(wifi_ssid);
        const char * psk = doc["wifi_pass"];
        serial_print(psk);
        // Reading wifi config
        String wifi_config = get_wifi_setting();
        // modifying wifi config
        serial_print(wifi_config);
        JsonDocument wifi_conf;
        deserializeJson(wifi_conf, wifi_config);
        wifi_conf["wifi_function"] = "STA";
        wifi_conf["wifi_ssid"] = wifi_ssid;
        wifi_conf["wifi_pass"] = psk;
        serializeJsonPretty(wifi_conf, wifi_config);
        serial_print(wifi_config);
        // writing wifi config
        save_wifi_settings(wifi_config);
        restart();
    }
    if(strcmp(request_type, "reset_device") == 0)
    {
        TickerForTimeOut.once_ms(100, reset_device);
    }
    if(strcmp(request_type, "restart_device") == 0)
    {
        TickerForTimeOut.once_ms(100, restart);
    }
    if(strcmp(request_type, "lora_transmit") == 0)
    {
        String msg = doc["data"];
        bool get_response = doc["get_response"];
        LoRa_sendMessage(msg);
        if(get_response)
            show_alert("LoRa msg transmitted successfully");
    }
}

void reset_device()
{
    show_alert("Device reset successfully,\nPlease reconfigure the device by connecting to the AP.\nRebooting Now.");
    clear_oled_display();
    display_text_oled("Resetting", 0, 10);
    serial_print("Stopping WiFi and tickers");
    if (WiFi.status() == WL_CONNECTED)
    {
        WiFi.disconnect();
    }
    TickerForBtnPresses.detach();
    TickerForDNSRequest.detach();
    TickerForLedNotification.detach();
    serial_print("Formatting SPIFFS");
    bool formatted = SPIFFS.format();
    if (formatted)
    {
        serial_print("Success formatting");
        restart();
    }
    else
    {
        serial_print("Error formatting");
    }
}

void show_alert(String msg)
{
    JsonDocument doc;
    doc["response_type"] = "alert";
    doc["alert_msg"] = msg;
    String return_response;
    serializeJsonPretty(doc, return_response);
    send_to_ws(return_response);
}

void restart()
{
    serial_print("Restarting esp.");
    ESP.restart();
}

void setup_mdns()
{
    String mac = WiFi.macAddress();
    mac.replace(":","_");
    hostname = "project-voyager-"+mac;
    WiFi.hostname(hostname);
    serial_print("Device hostname: "+hostname);
    if (MDNS.begin(hostname.c_str())) {
        MDNS.addService("http", "tcp", 80); 
        serial_print("Started mDNS service");
    }
}

void setup_dns()
{
    serial_print("DNS service started.");
    serial_print("Soft AP IP.");
    serial_print(WiFi.softAPIP().toString());
    dnsServer.setTTL(300);
    dnsServer.setErrorReplyCode(DNSReplyCode::ServerFailure);
    dnsServer.start(53, "*", WiFi.softAPIP());
    TickerForDNSRequest.attach_ms(10, dns_request_process);
}

void dns_request_process()
{
    dnsServer.processNextRequest();
}

void config_gpios()
{
    pinMode(LED, OUTPUT);
    pinMode(BTN1, INPUT_PULLDOWN);
    pinMode(BTN2, INPUT_PULLDOWN);
    digitalWrite(LED, LOW);
    setupTickers();
}

void serial_print(String msg)
{
    if(DEBUGGING)
    {
        // display_text_oled(msg, 0, 10);
        Serial.println(msg);
    }
}

void setupTickers()
{
    TickerForBtnPresses.attach_ms(10, btn_intrupt);
    transmit_beacon();
    TickerForLoraBeacon.attach(10, transmit_beacon);
}

void stop_nortify_led()
{
    TickerForLedNotification.detach();
    digitalWrite(LED, LOW);
}

void nortify_led()
{
    serial_print("Nortify");
    TickerForLedNotification.attach_ms(500, led_nortifier);
}

String device_becon()
{
    String wifi_mac = WiFi.macAddress();
    String project = "Voyager";
    String packet_type = "beacon";
    JsonDocument doc;
    doc["mac"] = wifi_mac;
    doc["project"] = "Voyager";
    doc["pack_type"] = packet_type;
    String return_string;
    serializeJson(doc, return_string);
    return return_string;
}