#ifndef wifi_support
    #define wifi_support
    #include <Arduino.h>
    #include <WiFi.h>
    #include <support_method.h>
    #include <ArduinoJson.h>
    #include <web_sockets.h>
    #include "mqtt_support.h"
    struct WiFiBackup{
        String backup_config = "";
        bool backup_done = false;
    };
    extern WiFiBackup wifi_backup;
    void config_wifi();
    String get_wifi_setting();
    void save_wifi_settings(String config);
    void wifi_monitor(void* param);
    void connect_wifi();
    void onWifiConnect(WiFiEvent_t event, WiFiEventInfo_t info);
    void onWifiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info);
    void setup_sta(const char* wifi_ssid, const char* wifi_pass);
    void setup_ap(const char* wifi_ssid);
    void scan_ssid(void* args);
#endif
