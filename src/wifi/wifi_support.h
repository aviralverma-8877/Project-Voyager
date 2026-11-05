#ifndef wifi_support
    #define wifi_support
    #include <Arduino.h>
    #include <WiFi.h>
    #include <core/support_method.h>
    #include <ArduinoJson.h>
    #include <web/web_sockets.h>
    #include "wifi_config.h"

    #define WIFI_CONFIG_PATH  "/config/wifi_config.json"
    #define WIFI_DEFAULT_PATH "/config/wifi_default.json"

    // Raw copy of wifi_config.json taken at boot; web_server restores it after a
    // LittleFS OTA via save_wifi_settings(). Contains secrets: never log it.
    struct WiFiBackup{
        String backup_config = "";
        bool backup_done = false;
    };
    extern WiFiBackup wifi_backup;
    extern bool WiFi_setup_done;

    // Boot / connectivity
    void config_wifi(void *param);           // task: load config, bring WiFi up, then vTaskDelete
    void connect_wifi();
    void setup_sta(const char* wifi_ssid, const char* wifi_pass);
    void setup_ap(const char* wifi_ssid, const char* wifi_pass, int channel = 1);  // always WPA2
    void wifi_monitor(void* param);          // task: STA connects (strongest BSSID), backoff, fallback AP, roam
    void onWifiConnect(WiFiEvent_t event, WiFiEventInfo_t info);    // non-blocking
    void onWifiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info); // non-blocking
    void scan_ssid(void* args);              // task: sends wifi_scan over WS (deduped, strongest first)
    String wifi_last_scan_json();            // last complete wifi_scan JSON, "" if none yet (any task)

    // Config files (Contract 5). No side effects, no logging of contents.
    String get_wifi_setting(String wifi_config_path);   // raw file text or ""
    WifiConfig wifi_load_config();                       // config -> default file -> compiled defaults
    bool save_wifi_settings(String config);              // atomic write only, never restarts

    // Contract 6 WiFi API (called by core's handle_operations). Each validates,
    // writes atomically and schedules exactly one restart; false + WS alert on bad input.
    bool wifi_save_sta(const char* ssid, const char* pass);
    bool wifi_switch_to_ap();
    bool wifi_set_ap_pass(const char* pass);   // "" = use wifi_default_ap_pass()
    String wifi_default_ap_pass();

    // WiFi setup over USB serial (Improv, used by the web flasher). Unlike
    // wifi_save_sta this connects *live* first, so the caller can report the device
    // URL: blocks up to timeout_ms. On success saves STA config, sets url_out to
    // "http://<ip>" and schedules one restart a few seconds later (clean STA boot).
    // On failure nothing is saved and the previous WiFi state is restored.
    bool wifi_improv_provision(const char* ssid, const char* pass, uint32_t timeout_ms, String& url_out);
    // Blocking scan for the flasher's network list: one entry per SSID (strongest),
    // strongest first, hidden SSIDs skipped. Returns the count (<= max).
    struct WifiNet { char ssid[33]; int8_t rssi; bool secure; };
    size_t wifi_improv_scan(WifiNet* out, size_t max);
    // "http://<ip>" while STA is connected, else "".
    String wifi_sta_url();
#endif
