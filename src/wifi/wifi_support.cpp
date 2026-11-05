#include "wifi_support.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include <new>

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
// Improv provisioning owns the STA interface; wifi_monitor stands still meanwhile.
static volatile bool g_provisioning = false;
static bool g_scan_running = false;

// Serialises every scan channel and WiFi.begin(): Arduino keeps one shared scan
// result buffer, and a connect issued mid-scan fails.
static SemaphoreHandle_t g_radio_lock = NULL;

// Connect attempt bookkeeping (monitor writes, event callbacks clear).
static volatile bool g_attempt_active = false;
static volatile uint32_t g_attempt_start = 0;
static volatile uint8_t g_last_pick_channel = 0;   // channel of the BSSID we last tried

// Last STA disconnect, written by onWifiDisconnect, logged/handled by wifi_monitor.
static volatile uint32_t g_disc_count = 0;
static volatile uint8_t g_disc_reason = 0;
static uint8_t g_disc_bssid[6] = {0};

#define WIFI_SCAN_CHANNELS        13      // 2.4 GHz channels 1..13
#define WIFI_SCAN_DWELL_MS        120     // active scan time per channel
#define WIFI_SCAN_HOME_MS         150     // back on the home channel between channels (AP / link up)
#define WIFI_SCAN_HOME_IDLE_MS    20      // nothing to serve: STA only and not connected
#define WIFI_ATTEMPT_TIMEOUT_MS   15000u  // a connect attempt with no result after this is dead
#define WIFI_FALLBACK_STOP_MS     10000u  // STA must be up this long before the fallback AP goes

static void copy_str(char* dst, size_t cap, const char* src)
{
    if (!src) src = "";
    strlcpy(dst, src, cap);
}

static SemaphoreHandle_t radio_lock()
{
    if (g_radio_lock == NULL)
    {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        bool used = false;
        portENTER_CRITICAL(&g_wifi_mux);
        if (g_radio_lock == NULL) { g_radio_lock = m; used = true; }
        portEXIT_CRITICAL(&g_wifi_mux);
        if (!used && m) vSemaphoreDelete(m);
    }
    return g_radio_lock;
}

static bool scan_running()
{
    bool r;
    portENTER_CRITICAL(&g_wifi_mux);
    r = g_scan_running;
    portEXIT_CRITICAL(&g_wifi_mux);
    return r;
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
    char buf[WIFI_HOSTNAME_LEN];
    wifi_format_hostname(mac, buf);
    hostname = buf;
    WiFi.setHostname(buf);
    serial_print("Device hostname: " + hostname);
}

// Modem sleep off (WIFI_PS_NONE). Arduino stores the setting and re-applies it on
// every STA start, but set it explicitly after each mode change as well: with the
// default WIFI_PS_MIN_MODEM the STA link showed 60-300 ms ping and dropped WS/SSE.
static void wifi_apply_no_sleep()
{
    WiFi.setSleep(false);
    if (WiFi.getMode() & WIFI_MODE_STA) esp_wifi_set_ps(WIFI_PS_NONE);
}

static const char* ps_name()
{
    wifi_ps_type_t ps = WIFI_PS_NONE;
    if (esp_wifi_get_ps(&ps) != ESP_OK) return "unknown";
    return ps == WIFI_PS_NONE ? "off" : "ON";
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
    // In AP+STA there is one radio channel. Start the AP on the channel the STA is
    // trying (last picked BSSID), so bringing the AP up never drags the radio away
    // from a connect attempt.
    int ch = g_last_pick_channel;
    if (ch < 1 || ch > 13) ch = WiFi.channel();
    if (ch < 1 || ch > 13) ch = 1;
    serial_print("STA not connected: starting fallback AP (AP+STA) on channel " + String(ch) + ". Saved network kept.");
    WiFi.mode(WIFI_AP_STA);
    wifi_apply_no_sleep();
    setup_ap(fallback_ap_ssid(), g_ap_pass, ch);
    g_fallback_ap = true;
}

static void stop_fallback_ap()
{
    if (!g_fallback_ap) return;
    serial_print("STA connected: stopping fallback AP.");
    WiFi.softAPdisconnect(true);   // AP off, mode back to STA (the STA link is kept)
    wifi_apply_no_sleep();
    g_fallback_ap = false;
    g_ap_ssid[0] = '\0';
    IPAddress ip = WiFi.localIP();
    oled_set_wifi(WIFI_CONNECTED_STA, g_sta_ssid, ip.toString().c_str());
}

// Improv: give the browser time to read the result before core's restart().
static void delayed_restart_task(void* param)
{
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)param));
    restart(NULL);   // waits 500 ms, ESP.restart(); never returns
    vTaskDelete(NULL);
}

// Exactly one restart per boot, no matter how many API calls request it.
// extra_delay_ms > 0 waits that long first (Improv answer must reach the browser).
static bool schedule_restart(uint32_t extra_delay_ms = 0)
{
    bool first;
    portENTER_CRITICAL(&g_wifi_mux);
    first = !g_restart_scheduled;
    g_restart_scheduled = true;
    portEXIT_CRITICAL(&g_wifi_mux);
    if (!first) return true;
    // core's restart() waits 500 ms, then ESP.restart().
    BaseType_t ok = extra_delay_ms
        ? xTaskCreatePinnedToCore(delayed_restart_task, "Restart", 4096, (void*)(uintptr_t)extra_delay_ms, 1, NULL, 1)
        : xTaskCreatePinnedToCore(restart, "Restart", 4096, NULL, 1, NULL, 1);
    if (ok != pdPASS)
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
    // core's read_file() holds the FS lock (refuses during a LittleFS-OTA remount).
    String text;
    if (!read_file(wifi_config_path.c_str(), text)) return "";
    if (text.length() == 0 || text.length() > 1024) return "";
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
    // The restart's boot connect picks the strongest BSSID (wifi_monitor).
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
// Improv (WiFi setup from the web flasher over USB serial)
// ---------------------------------------------------------------------------
String wifi_sta_url()
{
    if (!WiFi.isConnected()) return "";
    IPAddress ip = WiFi.localIP();
    if (ip == IPAddress(0, 0, 0, 0)) return "";
    return "http://" + ip.toString();
}

bool wifi_improv_provision(const char* ssid, const char* pass, uint32_t timeout_ms, String& url_out)
{
    if (!pass) pass = "";
    url_out = "";
    if (!wifi_valid_ssid(ssid) || !wifi_valid_sta_pass(pass)) return false;
    if (g_provisioning) return false;
    g_provisioning = true;   // wifi_monitor stands still from here on
    serial_print("Improv: trying network " + String(ssid));

    // Keep an AP up while trying (AP -> AP+STA) so a browser on the AP isn't cut off.
    wifi_mode_t prev = WiFi.getMode();
    SemaphoreHandle_t lock = radio_lock();
    bool locked = lock && xSemaphoreTake(lock, pdMS_TO_TICKS(5000)) == pdTRUE;
    if (prev & WIFI_MODE_STA)
    {
        WiFi.disconnect(false, false);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    else
    {
        WiFi.mode(prev == WIFI_MODE_AP ? WIFI_AP_STA : WIFI_STA);
    }
    WiFi.setAutoReconnect(false);
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);      // strongest node of a mesh
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    wifi_apply_no_sleep();
    WiFi.begin(ssid, pass);
    if (locked) xSemaphoreGive(lock);

    uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms)
    {
        if (WiFi.isConnected() && WiFi.localIP() != IPAddress(0, 0, 0, 0)) break;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    url_out = wifi_sta_url();
    if (url_out.length() > 0)
    {
        WifiConfig cfg = wifi_load_config();
        cfg.mode = WIFI_CFG_STA;
        cfg.ssid = ssid;
        cfg.pass = pass;
        String out;
        wifi_serialize_config(cfg, out);
        bool saved = write_file_atomic(WIFI_CONFIG_PATH, out);
        serial_print(saved ? "Improv: connected (" + url_out + "), config saved, restarting"
                           : "Improv: connected but saving the config failed, restarting");
        // Reboot into a clean STA setup (monitor, mDNS, no leftover AP). DHCP normally
        // hands the same IP back, so the URL we report stays valid. g_provisioning
        // stays set so the monitor doesn't interfere until then.
        schedule_restart(3000);
        if (!saved) url_out = "";
        return saved;
    }

    // Failed: back to how it was. Nothing was saved.
    serial_print("Improv: could not connect to " + String(ssid));
    WiFi.disconnect(false, false);
    if (!(prev & WIFI_MODE_STA)) WiFi.mode(prev);
    g_provisioning = false;
    if (g_monitor_handle) xTaskNotifyGive(g_monitor_handle);   // reconnects the saved network
    return false;
}

size_t wifi_improv_scan(WifiNet* out, size_t max)
{
    if (!out || max == 0 || g_provisioning) return 0;
    SemaphoreHandle_t lock = radio_lock();
    if (lock && xSemaphoreTake(lock, pdMS_TO_TICKS(5000)) != pdTRUE) return 0;
    portENTER_CRITICAL(&g_wifi_mux);
    g_scan_running = true;
    portEXIT_CRITICAL(&g_wifi_mux);
    wifi_mode_t prev = WiFi.getMode();
    if (!(prev & WIFI_MODE_STA)) WiFi.mode(prev == WIFI_MODE_AP ? WIFI_AP_STA : WIFI_STA);  // scans need STA

    size_t count = 0;
    int n = WiFi.scanNetworks(false, false, false, 300);
    for (int i = 0; i < n; i++)
    {
        String s = WiFi.SSID(i);
        if (s.length() == 0 || s.length() > 32) continue;   // hidden / invalid
        int rssi = WiFi.RSSI(i);
        bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        size_t j = 0;
        while (j < count && strcmp(out[j].ssid, s.c_str()) != 0) j++;
        if (j < count)
        {
            if (rssi > out[j].rssi) { out[j].rssi = (int8_t)rssi; out[j].secure = secure; }
            continue;
        }
        if (count == max) continue;
        copy_str(out[count].ssid, sizeof(out[count].ssid), s.c_str());
        out[count].rssi = (int8_t)(rssi < -128 ? -128 : rssi > 0 ? 0 : rssi);
        out[count].secure = secure;
        count++;
    }
    WiFi.scanDelete();
    if (!(prev & WIFI_MODE_STA)) WiFi.mode(prev);
    portENTER_CRITICAL(&g_wifi_mux);
    g_scan_running = false;
    portEXIT_CRITICAL(&g_wifi_mux);
    if (lock) xSemaphoreGive(lock);

    // strongest first (insertion sort, n <= max)
    for (size_t a = 1; a < count; a++)
    {
        WifiNet e = out[a];
        size_t b = a;
        while (b > 0 && out[b - 1].rssi < e.rssi) { out[b] = out[b - 1]; b--; }
        out[b] = e;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Channel-by-channel scan
// ---------------------------------------------------------------------------
// Scans channels 1..13 one at a time (optionally only for `ssid`), calling on_hit(i)
// for each result index while the Arduino result buffer is valid. Between channels
// the radio returns to the home channel for home_ms, so an AP or a live STA link
// keeps passing traffic. Returns the number of channels that scanned OK.
template <typename F>
static int scan_channels(const char* ssid, uint32_t home_ms, F on_hit)
{
    SemaphoreHandle_t lock = radio_lock();
    int ok_channels = 0;
    for (uint8_t ch = 1; ch <= WIFI_SCAN_CHANNELS; ch++)
    {
        if (lock && xSemaphoreTake(lock, pdMS_TO_TICKS(3000)) != pdTRUE) continue;
        int n = WiFi.scanNetworks(false, false, false, WIFI_SCAN_DWELL_MS, ch, ssid);
        if (n < 0)
        {
            // Driver busy (e.g. a connect still in progress): one retry on this channel.
            WiFi.scanComplete();   // clears a stale "scanning" state after its timeout
            WiFi.scanDelete();
            vTaskDelay(pdMS_TO_TICKS(200));
            n = WiFi.scanNetworks(false, false, false, WIFI_SCAN_DWELL_MS, ch, ssid);
            if (n < 0) WiFi.scanComplete();
        }
        else
        {
            ok_channels++;
            for (int i = 0; i < n; i++) on_hit(i);
        }
        WiFi.scanDelete();
        if (lock) xSemaphoreGive(lock);
        vTaskDelay(pdMS_TO_TICKS(home_ms));
    }
    return ok_channels;
}

static void log_candidate(const char* what, const WifiApCandidate& c)
{
    char b[18];
    wifi_format_bssid(c.bssid, b);
    serial_print(String(what) + " BSSID " + b + ", channel " + String(c.channel) +
                 ", RSSI " + String(c.rssi) + " dBm");
}

static void sta_begin_pinned(const WifiApCandidate& c)
{
    SemaphoreHandle_t lock = radio_lock();
    bool locked = lock && xSemaphoreTake(lock, pdMS_TO_TICKS(3000)) == pdTRUE;
    g_last_pick_channel = c.channel;
    g_attempt_start = millis();
    g_attempt_active = true;
    WiFi.begin(g_sta_ssid, g_sta_pass, c.channel, c.bssid);
    if (locked) xSemaphoreGive(lock);
}

// Scans for the saved SSID and connects to its strongest BSSID, skipping `avoid`
// (the node that just failed) unless it is the only one. No match (hidden SSID,
// scan error): plain WiFi.begin, where the driver's all-channel scan sorted by
// signal still picks the strongest node.
static void sta_connect_best(const uint8_t* avoid)
{
    // The driver refuses to scan while the STA is connecting. Arduino 2.0.x starts a
    // connect on its own after the first disconnect of a boot (its "first_connect"
    // retry, regardless of setAutoReconnect(false)), so cancel any attempt we did
    // not start, then give the driver a moment to go idle. Saved config untouched.
    if (!WiFi.isConnected())
    {
        WiFi.disconnect(false, false);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    uint32_t home = (g_fallback_ap || WiFi.isConnected()) ? WIFI_SCAN_HOME_MS : WIFI_SCAN_HOME_IDLE_MS;
    WifiApSelector sel;
    sel.set_avoid(avoid);
    uint32_t t0 = millis();
    auto on_hit = [&](int i) {
        sel.add(g_sta_ssid, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.BSSID(i), WiFi.channel(i));
    };
    int ok = scan_channels(g_sta_ssid, home, on_hit);
    if (ok == 0)
    {
        serial_print("Connect scan: driver busy on every channel, rescanning once");
        vTaskDelay(pdMS_TO_TICKS(1000));
        ok = scan_channels(g_sta_ssid, home, on_hit);
    }
    const WifiApCandidate& best = sel.pick();
    if (best.valid)
    {
        String what = "Connecting to " + String(g_sta_ssid) + " (" + String(sel.matches) + " BSSIDs, " +
                      String(ok) + "/" + String(WIFI_SCAN_CHANNELS) + " channels, scan " +
                      String(millis() - t0) + " ms): ";
        what += sel.only_avoided() ? "only node seen is the one that just failed, retrying it:" : "chosen";
        log_candidate(what.c_str(), best);
        sta_begin_pinned(best);
        return;
    }
    serial_print("Connecting to " + String(g_sta_ssid) + ": no BSSID found (" + String(ok) + "/" +
                 String(WIFI_SCAN_CHANNELS) + " channels scanned), plain connect");
    SemaphoreHandle_t lock = radio_lock();
    bool locked = lock && xSemaphoreTake(lock, pdMS_TO_TICKS(3000)) == pdTRUE;
    g_last_pick_channel = 0;
    g_attempt_start = millis();
    g_attempt_active = true;
    WiFi.begin(g_sta_ssid, g_sta_pass);
    if (locked) xSemaphoreGive(lock);
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
    WiFi.setSleep(false);         // stored; applied when STA starts
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
        wifi_apply_no_sleep();
        setup_ap(cfg.ssid.c_str(), g_ap_pass);
    }
}

// Starts STA mode and the monitor, which owns every connect attempt (the first one
// included), the fallback AP and the reconnect backoff. Waits up to 20 s here only
// to show progress on the OLED before boot continues.
void setup_sta(const char* wifi_ssid, const char* wifi_pass)
{
    g_sta_mode = true;
    if (wifi_ssid != g_sta_ssid) copy_str(g_sta_ssid, sizeof(g_sta_ssid), wifi_ssid);
    if (wifi_pass != g_sta_pass) copy_str(g_sta_pass, sizeof(g_sta_pass), wifi_pass);
    serial_print("Connecting to WiFi: " + String(g_sta_ssid));
    WiFi.setAutoReconnect(false);   // wifi_monitor owns retries and their backoff
    WiFi.mode(WIFI_STA);
    wifi_apply_no_sleep();
    // Fallback for plain begin(): sweep all channels and take the strongest node
    // (Arduino's default WIFI_FAST_SCAN joins the first matching node it hears).
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    oled_set_wifi(WIFI_CONNECTING, g_sta_ssid, "");

    if (g_monitor_handle == NULL &&
        xTaskCreatePinnedToCore(wifi_monitor, "wifi_monitor", 6144, NULL, 1, &g_monitor_handle, 1) != pdPASS)
    {
        g_monitor_handle = NULL;
        serial_print("wifi_monitor task create failed: single plain connect");
        WiFi.begin(g_sta_ssid, g_sta_pass);
    }

    char status[24];
    for (int count = WIFI_STA_CONNECT_TIMEOUT_S; count > 0 && !WiFi.isConnected(); count--)
    {
        snprintf(status, sizeof(status), "Connecting %ds", count);
        oled_status(status);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    oled_status(WiFi.isConnected() ? "WiFi connected" : "WiFi not connected yet");
}

void setup_ap(const char* wifi_ssid, const char* wifi_pass, int channel)
{
    const char* ssid = wifi_valid_ssid(wifi_ssid) ? wifi_ssid : WIFI_DEFAULT_AP_SSID;
    String pass = (wifi_pass && wifi_valid_ap_pass(wifi_pass)) ? String(wifi_pass) : wifi_default_ap_pass();
    if (channel < 1 || channel > 13) channel = 1;

    serial_print("Configuring Access Point: " + String(ssid) + " (channel " + String(channel) + ")");
    const IPAddress localIP(192, 168, 4, 1);
    const IPAddress gatewayIP(192, 168, 4, 1);
    const IPAddress subnetMask(255, 255, 255, 0);
    WiFi.softAPConfig(localIP, gatewayIP, subnetMask);

    bool ok = WiFi.softAP(ssid, pass.c_str(), channel);   // WPA2-PSK (pass is always 8-63)
    if (!ok)
    {
        serial_print("softAP failed, retrying with default SSID and password");
        ssid = WIFI_DEFAULT_AP_SSID;
        pass = wifi_default_ap_pass();
        ok = WiFi.softAP(ssid, pass.c_str(), channel);
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

// STA mode only. Owns every connect attempt:
// - first connect right away, then 5..60 s backoff while down;
// - every attempt rescans and pins the strongest BSSID; after a "bad AP" disconnect
//   (AUTH_EXPIRE, BEACON_TIMEOUT, NO_AP_FOUND, ...) the failed node is avoided once;
// - an attempt still in progress (< 15 s, no result) is never interrupted;
// - fallback AP (this boot only) after 20 s down, but not over a live attempt;
//   it stops once STA has been up for 10 s;
// - optional roam: on a settled link below WIFI_ROAM_SCAN_BELOW_DBM, at most every
//   5 min and never while the serial bridge is on, move to a node of the same SSID
//   that is >= WIFI_ROAM_MIN_GAIN_DB stronger.
// Never writes config.
void wifi_monitor(void* param)
{
    uint32_t backoff = WIFI_BACKOFF_MIN_MS;
    bool was_up = false;
    bool retry_now = true;                 // boot: connect immediately
    uint32_t now = millis();
    uint32_t down_since = now;
    uint32_t last_attempt = now;
    uint32_t up_since = now;
    uint32_t last_roam_check = now;
    uint32_t seen_disc = 0;
    bool have_avoid = false;
    uint8_t avoid[6] = {0};

    for (;;)
    {
        // Woken early by onWifiConnect/onWifiDisconnect; otherwise poll every second.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        now = millis();

        // Improv provisioning owns the STA interface: stand still, and forget the
        // disconnects it caused (they are not failures of the saved network).
        if (g_provisioning)
        {
            seen_disc = g_disc_count;
            last_attempt = now;
            continue;
        }

        // Disconnect events (logged here, not in the event callback).
        uint32_t dc = g_disc_count;
        if (dc != seen_disc)
        {
            uint8_t reason, b[6];
            portENTER_CRITICAL(&g_wifi_mux);
            reason = g_disc_reason;
            memcpy(b, g_disc_bssid, 6);
            portEXIT_CRITICAL(&g_wifi_mux);
            char bs[18];
            wifi_format_bssid(b, bs);
            WifiDiscKind kind = wifi_classify_reason(reason);
            String msg = "WiFi disconnected: reason " + String(reason) + " " + wifi_reason_name(reason) +
                         ", BSSID " + bs;
            if (dc - seen_disc > 1) msg += " (" + String(dc - seen_disc) + " events)";
            if (kind == WIFI_DISC_BAD_AP)
            {
                static const uint8_t zero[6] = {0};
                if (memcmp(b, zero, 6) != 0) { memcpy(avoid, b, 6); have_avoid = true; msg += ", trying another node next"; }
            }
            else if (kind == WIFI_DISC_AUTH)
            {
                msg += ", check the WiFi password";
            }
            serial_print(msg);
            seen_disc = dc;
        }

        if (WiFi.isConnected())
        {
            if (!was_up)
            {
                up_since = now;
                last_roam_check = now;
                have_avoid = false;
                char bs[18];
                wifi_format_bssid(WiFi.BSSID(), bs);
                serial_print("STA connected: BSSID " + String(bs) + ", channel " + String(WiFi.channel()) +
                             ", RSSI " + String(WiFi.RSSI()) + " dBm, IP " + WiFi.localIP().toString() +
                             ", power save " + ps_name());
                setup_mdns();
            }
            was_up = true;
            backoff = WIFI_BACKOFF_MIN_MS;
            if (g_fallback_ap && now - up_since >= WIFI_FALLBACK_STOP_MS) stop_fallback_ap();

            int rssi = WiFi.RSSI();
            // Never scan/roam during a serial (YMODEM) transfer: the channel hops and a
            // reconnect would stall the page's WebSocket mid-transfer.
            if (!g_fallback_ap && !lora_serial && !scan_running() &&
                wifi_roam_check_due(rssi, now - up_since, now - last_roam_check))
            {
                WifiApSelector sel;
                scan_channels(g_sta_ssid, WIFI_SCAN_HOME_MS, [&](int i) {
                    sel.add(g_sta_ssid, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.BSSID(i), WiFi.channel(i));
                });
                last_roam_check = millis();
                uint8_t cur[6];
                const uint8_t* cb = WiFi.BSSID();
                if (cb) memcpy(cur, cb, 6); else memset(cur, 0, 6);
                rssi = WiFi.RSSI();
                if (WiFi.isConnected() && wifi_should_roam(rssi, sel.any, cur))
                {
                    log_candidate(("Roaming: current " + String(rssi) + " dBm, stronger node").c_str(), sel.any);
                    sta_begin_pinned(sel.any);
                }
            }
            continue;
        }

        if (was_up)
        {
            serial_print("STA link lost");
            was_up = false;
            down_since = now;
            backoff = WIFI_BACKOFF_MIN_MS;
            retry_now = true;       // first reconnect right away, then back off
        }

        bool attempt_live = g_attempt_active && (now - g_attempt_start) < WIFI_ATTEMPT_TIMEOUT_MS;

        if (!g_fallback_ap && !attempt_live && now - down_since >= WIFI_STA_CONNECT_TIMEOUT_S * 1000u)
            start_fallback_ap();

        if (!attempt_live && (retry_now || now - last_attempt >= backoff))
        {
            if (!retry_now)
            {
                backoff = wifi_next_backoff(backoff);
                serial_print("Retrying " + String(g_sta_ssid) + " (next retry in " + String(backoff / 1000) + "s)");
            }
            retry_now = false;
            sta_connect_best(have_avoid ? avoid : NULL);
            have_avoid = false;     // avoid a failed node for one pick only
            last_attempt = millis();
        }
    }
}

// ---------------------------------------------------------------------------
// Event callbacks (Arduino event task): no file I/O, no I2C, no blocking, no logging.
// ---------------------------------------------------------------------------
void onWifiConnect(WiFiEvent_t event, WiFiEventInfo_t info)
{
    // ARDUINO_EVENT_WIFI_STA_GOT_IP: STA only.
    g_attempt_active = false;
    IPAddress ip(info.got_ip.ip_info.ip.addr);
    oled_set_wifi(WIFI_CONNECTED_STA, g_sta_ssid, ip.toString().c_str());
    if (g_monitor_handle) xTaskNotifyGive(g_monitor_handle);
}

void onWifiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info)
{
    // Our own leave (WiFi.disconnect / begin with a new BSSID / roam): not a failed
    // attempt, so it must not clear the in-flight flag of the attempt that follows.
    if (wifi_classify_reason(info.wifi_sta_disconnected.reason) == WIFI_DISC_SELF) return;
    g_attempt_active = false;
    portENTER_CRITICAL(&g_wifi_mux);
    g_disc_reason = info.wifi_sta_disconnected.reason;
    memcpy(g_disc_bssid, info.wifi_sta_disconnected.bssid, 6);
    g_disc_count = g_disc_count + 1;
    portEXIT_CRITICAL(&g_wifi_mux);
    if (!g_fallback_ap)
        oled_set_wifi(WIFI_CONNECTING, g_sta_ssid, "");
    if (g_monitor_handle) xTaskNotifyGive(g_monitor_handle);
}

// ---------------------------------------------------------------------------
// Scan for the UI (task)
// ---------------------------------------------------------------------------
#define WIFI_SCAN_MAX_RESULTS 40

static String g_last_scan;                       // last wifi_scan JSON, under g_scan_lock
static SemaphoreHandle_t g_scan_lock = NULL;     // created by the (single) scan task

String wifi_last_scan_json()
{
    String copy;
    if (g_scan_lock && xSemaphoreTake(g_scan_lock, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        copy = g_last_scan;
        xSemaphoreGive(g_scan_lock);
    }
    return copy;
}

// Sends {"response_type":"wifi_scan","SSID":[{ssid,rssi}]}: each SSID once with
// its strongest signal, strongest first, hidden networks left out. The scan goes
// channel by channel so the AP / STA link keeps working (one blocking all-channel
// scan at 300 ms/channel stalled the AP for ~7 s on the bench).
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
    if (g_scan_lock == NULL) g_scan_lock = xSemaphoreCreateMutex();   // only one scan task at a time

    // ~1.4 KB, heap only while a scan runs (keeps it off the 6000-byte task stack).
    WifiScanEntry* list = new (std::nothrow) WifiScanEntry[WIFI_SCAN_MAX_RESULTS];
    if (list == NULL)
    {
        serial_print("scan_ssid: out of memory");
        show_alert("WiFi scan failed. Please try again.");
        portENTER_CRITICAL(&g_wifi_mux);
        g_scan_running = false;
        portEXIT_CRITICAL(&g_wifi_mux);
        vTaskDelete(NULL);
        return;
    }
    uint32_t t_start = millis();
    wifi_mode_t prev_mode = WiFi.getMode();
    size_t count = 0;
    int hits = 0;

    int ok_channels = scan_channels(NULL, WIFI_SCAN_HOME_MS, [&](int i) {
        hits++;
        count = wifi_scan_merge(list, count, WIFI_SCAN_MAX_RESULTS, WiFi.SSID(i).c_str(), WiFi.RSSI(i));
    });
    // scanNetworks() turns STA on; give an AP-only device its mode back.
    if (prev_mode == WIFI_MODE_AP) WiFi.enableSTA(false);
    wifi_scan_sort(list, count);

    serial_print("WiFi networks found: " + String((int)count) + " (" + String(hits) + " hits, " +
                 String(ok_channels) + "/" + String(WIFI_SCAN_CHANNELS) + " channels, " +
                 String(millis() - t_start) + " ms)");

    String out;
    {
        JsonDocument doc;
        doc["response_type"] = "wifi_scan";
        JsonArray array = doc["SSID"].to<JsonArray>();
        for (size_t i = 0; i < count; i++)
        {
            JsonObject r = array.add<JsonObject>();
            r["ssid"] = (const char*)list[i].ssid;
            r["rssi"] = list[i].rssi;
        }
        serializeJson(doc, out);
    }
    delete[] list;
    if (ok_channels > 0 && g_scan_lock && xSemaphoreTake(g_scan_lock, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        g_last_scan = out;
        xSemaphoreGive(g_scan_lock);
    }
    send_to_ws(out);
    if (ok_channels == 0) show_alert("WiFi scan failed. Please try again.");

    portENTER_CRITICAL(&g_wifi_mux);
    g_scan_running = false;
    portEXIT_CRITICAL(&g_wifi_mux);
    vTaskDelete(NULL);
}
