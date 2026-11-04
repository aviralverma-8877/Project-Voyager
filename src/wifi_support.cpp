#include <wifi_support.h>
#include "esp_system.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
WiFiBackup wifi_backup;
bool WiFi_setup_done = false;

// Cached at boot by config_wifi (before any STA/AP event can fire) and only
// read afterwards, so the event callbacks never touch the filesystem.
static char g_sta_ssid[33] = "";
static char g_sta_pass[64] = "";
static char g_ap_pass[64] = "";      // "" = derived default
static char g_ap_ssid[33] = "";      // SSID the AP is actually running with

static volatile bool g_sta_mode = false;      // configured mode is STA
static volatile bool g_fallback_ap = false;   // AP running alongside STA (this boot only)
static TaskHandle_t g_monitor_handle = NULL;

static portMUX_TYPE g_wifi_mux = portMUX_INITIALIZER_UNLOCKED;
static bool g_restart_scheduled = false;
static bool g_scan_running = false;

static void copy_str(char* dst, size_t cap, const char* src)
{
    if (!src) src = "";
    strlcpy(dst, src, cap);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
String wifi_default_ap_pass()
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    return String(wifi_derive_ap_pass(mac).c_str());
}

// Must run before WiFi.mode()/begin() so DHCP and mDNS use the right name.
static void wifi_init_hostname()
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[40];
    snprintf(buf, sizeof(buf), "project-voyager-%02X_%02X_%02X_%02X_%02X_%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    hostname = buf;
    WiFi.setHostname(buf);
    serial_print("Device hostname: " + hostname);
}

static const char* fallback_ap_ssid()
{
    // Same SSID choice as "switch to AP" from the UI: the username, else the default.
    static char ssid[33];
    String uname = get_username();   // core: copy under lock
    if (wifi_valid_ssid(uname.c_str()))
        copy_str(ssid, sizeof(ssid), uname.c_str());
    else
        copy_str(ssid, sizeof(ssid), WIFI_DEFAULT_AP_SSID);
    return ssid;
}

static void start_fallback_ap()
{
    if (g_fallback_ap) return;
    serial_print("STA not connected: starting fallback AP (AP+STA). Saved network kept.");
    WiFi.mode(WIFI_AP_STA);
    setup_ap(fallback_ap_ssid(), g_ap_pass);
    g_fallback_ap = true;
}

static void stop_fallback_ap()
{
    if (!g_fallback_ap) return;
    serial_print("STA connected: stopping fallback AP.");
    WiFi.softAPdisconnect(true);   // AP off, mode back to STA
    g_fallback_ap = false;
    g_ap_ssid[0] = '\0';
    IPAddress ip = WiFi.localIP();
    oled_set_wifi(WIFI_CONNECTED_STA, g_sta_ssid, ip.toString().c_str());
}

// Exactly one restart per boot, no matter how many API calls request it.
static bool schedule_restart()
{
    bool first;
    portENTER_CRITICAL(&g_wifi_mux);
    first = !g_restart_scheduled;
    g_restart_scheduled = true;
    portEXIT_CRITICAL(&g_wifi_mux);
    if (!first) return true;
    // core's restart() waits 500 ms, then ESP.restart().
    if (xTaskCreatePinnedToCore(restart, "Restart", 4096, NULL, 1, NULL, 1) != pdPASS)
    {
        portENTER_CRITICAL(&g_wifi_mux);
        g_restart_scheduled = false;
        portEXIT_CRITICAL(&g_wifi_mux);
        return false;
    }
    return true;
}

static bool write_config_and_restart(const WifiConfig& cfg)
{
    String out;
    wifi_serialize_config(cfg, out);
    if (!write_file_atomic(WIFI_CONFIG_PATH, out))
    {
        serial_print("WiFi config write failed");
        show_alert("Could not save WiFi settings.");
        return false;
    }
    serial_print("WiFi config saved");
    if (!schedule_restart())
    {
        show_alert("WiFi settings saved. Restart the device to apply them.");
        return true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Config files (Contract 5)
// ---------------------------------------------------------------------------
String get_wifi_setting(String wifi_config_path)
{
    if (!LittleFS.exists(wifi_config_path)) return "";
    File file = LittleFS.open(wifi_config_path, FILE_READ);
    if (!file) return "";
    size_t size = file.size();
    if (size == 0 || size > 1024) { file.close(); return ""; }
    String text;
    text.reserve(size + 1);
    while (file.available()) text += (char)file.read();
    file.close();
    return text;
}

WifiConfig wifi_load_config()
{
    WifiConfig cfg;
    String raw = get_wifi_setting(WIFI_CONFIG_PATH);
    WifiParseResult r = wifi_parse_config(raw.c_str(), raw.length(), cfg);
    if (r != WIFI_PARSE_ERROR)
    {
        if (r == WIFI_PARSE_DEFAULTED) serial_print("wifi_config.json: some fields invalid, defaults used");
        return cfg;
    }
    serial_print("wifi_config.json missing or unreadable, trying wifi_default.json");
    raw = get_wifi_setting(WIFI_DEFAULT_PATH);
    if (wifi_parse_config(raw.c_str(), raw.length(), cfg) != WIFI_PARSE_ERROR) return cfg;
    serial_print("wifi_default.json missing or unreadable, using compiled defaults");
    return WifiConfig();
}

// Used by web_server to restore the boot-time config after a LittleFS OTA.
bool save_wifi_settings(String config)
{
    bool ok = write_file_atomic(WIFI_CONFIG_PATH, config);
    serial_print(ok ? "WiFi config restored" : "WiFi config restore failed");
    return ok;
}

// ---------------------------------------------------------------------------
// Contract 6 WiFi API
// ---------------------------------------------------------------------------
bool wifi_save_sta(const char* ssid, const char* pass)
{
    if (!pass) pass = "";
    if (!wifi_valid_ssid(ssid))
    {
        show_alert("Invalid SSID (1-32 characters).");
        return false;
    }
    if (!wifi_valid_sta_pass(pass))
    {
        show_alert("Invalid WiFi password (empty or 8-63 characters).");
        return false;
    }
    WifiConfig cfg = wifi_load_config();
    cfg.mode = WIFI_CFG_STA;
    cfg.ssid = ssid;
    cfg.pass = pass;
    serial_print("Saving STA network: " + String(ssid));
    return write_config_and_restart(cfg);
}

bool wifi_switch_to_ap()
{
    WifiConfig cfg = wifi_load_config();
    cfg.mode = WIFI_CFG_AP;
    cfg.ssid = fallback_ap_ssid();
    cfg.pass = "";   // the STA network is being replaced by an explicit user choice
    serial_print("Switching to AP mode: " + String(cfg.ssid.c_str()));
    return write_config_and_restart(cfg);
}

bool wifi_set_ap_pass(const char* pass)
{
    if (!wifi_valid_ap_pass_setting(pass))
    {
        show_alert("Invalid AP password (8-63 characters, or empty for the default).");
        return false;
    }
    WifiConfig cfg = wifi_load_config();
    cfg.ap_pass = pass;
    serial_print(pass[0] ? "Saving new AP password" : "Resetting AP password to default");
    return write_config_and_restart(cfg);
}

// ---------------------------------------------------------------------------
// Bring-up
// ---------------------------------------------------------------------------
void config_wifi(void *param)
{
    connect_wifi();
    WiFi_setup_done = true;
    vTaskDelete(NULL);
}

void connect_wifi()
{
    // Called once per boot from config_wifi. Reconnects are wifi_monitor's job.
    String raw = get_wifi_setting(WIFI_CONFIG_PATH);
    if (raw.length() > 0)
    {
        wifi_backup.backup_config = raw;
        wifi_backup.backup_done = true;
    }
    WifiConfig cfg = wifi_load_config();
    copy_str(g_sta_ssid, sizeof(g_sta_ssid), cfg.ssid.c_str());
    copy_str(g_sta_pass, sizeof(g_sta_pass), cfg.pass.c_str());
    copy_str(g_ap_pass, sizeof(g_ap_pass), cfg.ap_pass.c_str());

    WiFi.persistent(false);       // never let the driver write credentials to NVS
    wifi_init_hostname();         // before WiFi.mode()/begin()

    if (cfg.mode == WIFI_CFG_STA)
    {
        serial_print("STA Mode");
        setup_sta(g_sta_ssid, g_sta_pass);
    }
    else
    {
        serial_print("AP Mode");
        g_sta_mode = false;
        WiFi.mode(WIFI_AP);
        setup_ap(cfg.ssid.c_str(), g_ap_pass);
    }
}

void setup_sta(const char* wifi_ssid, const char* wifi_pass)
{
    g_sta_mode = true;
    serial_print("Connecting to WiFi: " + String(wifi_ssid));
    WiFi.setAutoReconnect(false);   // wifi_monitor owns retries and their backoff
    WiFi.mode(WIFI_STA);
    oled_set_wifi(WIFI_CONNECTING, wifi_ssid, "");
    WiFi.begin(wifi_ssid, wifi_pass);

    char status[24];
    for (int count = WIFI_STA_CONNECT_TIMEOUT_S; count > 0 && !WiFi.isConnected(); count--)
    {
        snprintf(status, sizeof(status), "Connecting %ds", count);
        oled_status(status);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    if (WiFi.isConnected())
    {
        oled_status("WiFi connected");
        setup_mdns();
    }
    else
    {
        oled_status("WiFi failed, AP on");
        start_fallback_ap();     // this boot only; nothing is written
    }

    if (g_monitor_handle == NULL)
    {
        if (xTaskCreatePinnedToCore(wifi_monitor, "wifi_monitor", 4096, NULL, 1, &g_monitor_handle, 1) != pdPASS)
        {
            g_monitor_handle = NULL;
            serial_print("wifi_monitor task create failed");
        }
    }
}

void setup_ap(const char* wifi_ssid, const char* wifi_pass)
{
    const char* ssid = wifi_valid_ssid(wifi_ssid) ? wifi_ssid : WIFI_DEFAULT_AP_SSID;
    String pass = (wifi_pass && wifi_valid_ap_pass(wifi_pass)) ? String(wifi_pass) : wifi_default_ap_pass();

    serial_print("Configuring Access Point: " + String(ssid));
    const IPAddress localIP(192, 168, 4, 1);
    const IPAddress gatewayIP(192, 168, 4, 1);
    const IPAddress subnetMask(255, 255, 255, 0);
    WiFi.softAPConfig(localIP, gatewayIP, subnetMask);

    bool ok = WiFi.softAP(ssid, pass.c_str());   // WPA2-PSK (pass is always 8-63)
    if (!ok)
    {
        serial_print("softAP failed, retrying with default SSID and password");
        ssid = WIFI_DEFAULT_AP_SSID;
        pass = wifi_default_ap_pass();
        ok = WiFi.softAP(ssid, pass.c_str());
    }
    if (!ok)
    {
        serial_print("softAP failed");
        oled_alert("WiFi", "AP start failed", 5000);
        return;
    }

    copy_str(g_ap_ssid, sizeof(g_ap_ssid), ssid);
    String ip = WiFi.softAPIP().toString();
    serial_print("AP IP: " + ip);
    oled_set_ap(ssid, pass.c_str(), ip.c_str());
    oled_set_wifi(WIFI_CONNECTED_AP, ssid, ip.c_str());
    setup_dns();
    setup_mdns();
}

// Runs only in STA mode. Retries the saved network with 5..60 s backoff and
// keeps a fallback AP up while STA has been down for 20 s. Never writes config.
void wifi_monitor(void* param)
{
    uint32_t backoff = WIFI_BACKOFF_MIN_MS;
    bool was_up = WiFi.isConnected();
    uint32_t down_since = millis();
    uint32_t last_attempt = millis();

    for (;;)
    {
        // Woken early by onWifiConnect/onWifiDisconnect; otherwise poll every second.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        uint32_t now = millis();

        if (WiFi.isConnected())
        {
            if (!was_up)
            {
                serial_print("STA connected");
                setup_mdns();
            }
            stop_fallback_ap();
            was_up = true;
            backoff = WIFI_BACKOFF_MIN_MS;
            continue;
        }

        if (was_up)
        {
            serial_print("STA link lost");
            was_up = false;
            down_since = now;
            last_attempt = now;
            backoff = WIFI_BACKOFF_MIN_MS;
        }

        if (!g_fallback_ap && now - down_since >= WIFI_STA_CONNECT_TIMEOUT_S * 1000u)
            start_fallback_ap();

        if (now - last_attempt >= backoff)
        {
            serial_print("Retrying " + String(g_sta_ssid) + " (next retry in " + String(wifi_next_backoff(backoff) / 1000) + "s)");
            if (!WiFi.reconnect())
                WiFi.begin(g_sta_ssid, g_sta_pass);
            last_attempt = now;
            backoff = wifi_next_backoff(backoff);
        }
    }
}

// ---------------------------------------------------------------------------
// Event callbacks (Arduino event task): no file I/O, no I2C, no blocking.
// ---------------------------------------------------------------------------
void onWifiConnect(WiFiEvent_t event, WiFiEventInfo_t info)
{
    // ARDUINO_EVENT_WIFI_STA_GOT_IP: STA only.
    IPAddress ip(info.got_ip.ip_info.ip.addr);
    String ip_s = ip.toString();
    oled_set_wifi(WIFI_CONNECTED_STA, g_sta_ssid, ip_s.c_str());
    serial_print("WiFi connected, IP " + ip_s);
    if (g_monitor_handle) xTaskNotifyGive(g_monitor_handle);
}

void onWifiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info)
{
    if (!g_fallback_ap)
        oled_set_wifi(WIFI_CONNECTING, g_sta_ssid, "");
    serial_print("WiFi disconnected, reason " + String(info.wifi_sta_disconnected.reason));
    if (g_monitor_handle) xTaskNotifyGive(g_monitor_handle);
}

// ---------------------------------------------------------------------------
// Scan (task)
// ---------------------------------------------------------------------------
#define WIFI_SCAN_MAX_RESULTS 40

void scan_ssid(void* args)
{
    bool busy;
    portENTER_CRITICAL(&g_wifi_mux);
    busy = g_scan_running;
    g_scan_running = true;
    portEXIT_CRITICAL(&g_wifi_mux);
    if (busy)
    {
        // The running scan broadcasts its result to every WS client.
        serial_print("scan_ssid: scan already running, request ignored");
        vTaskDelete(NULL);
        return;
    }

    wifi_mode_t prev_mode = WiFi.getMode();
    int n = WiFi.scanNetworks();
    if (n < 0)
    {
        WiFi.scanDelete();
        vTaskDelay(500 / portTICK_PERIOD_MS);
        n = WiFi.scanNetworks();
    }
    serial_print("WiFi networks found: " + String(n));

    JsonDocument doc;
    doc["response_type"] = "wifi_scan";
    JsonArray array = doc["SSID"].to<JsonArray>();
    int count = n < 0 ? 0 : (n > WIFI_SCAN_MAX_RESULTS ? WIFI_SCAN_MAX_RESULTS : n);
    for (int i = 0; i < count; i++)
    {
        JsonObject r = array.add<JsonObject>();
        r["ssid"] = WiFi.SSID(i);
        r["rssi"] = WiFi.RSSI(i);
    }
    WiFi.scanDelete();
    // scanNetworks() turns STA on; give an AP-only device its mode back.
    if (prev_mode == WIFI_MODE_AP) WiFi.enableSTA(false);

    String out;
    serializeJson(doc, out);
    doc.clear();
    send_to_ws(out);
    if (n < 0) show_alert("WiFi scan failed. Please try again.");

    portENTER_CRITICAL(&g_wifi_mux);
    g_scan_running = false;
    portEXIT_CRITICAL(&g_wifi_mux);
    vTaskDelete(NULL);
}
