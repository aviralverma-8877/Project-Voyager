#include "support_method.h"
#include "core_dispatch.h"
#include "bt_serial.h"
#include "improv_serial.h"
#include "net_serial.h"
#include <web/web_sockets.h>
#include <web/auth.h>
#include <web/web_server.h>
#include <sensors/sensor_support.h>
#include <mutex>

volatile bool lora_serial = false;
volatile bool fs_mounted = false;
String hostname;
DNSServer dnsServer;
TaskHandle_t debug_handler = NULL;

static String s_username;
static std::mutex s_username_lock;

// Halts boot with an OLED alert. Used for critical task/queue failures.
static void halt_boot(const char* text)
{
    Serial.printf("FATAL: Boot error: %s\n", text);
    oled_alert("Boot error", text, 0);
    while (true)
        vTaskDelay(pdMS_TO_TICKS(1000));
}

// ------------------------------------------------------------------ file I/O

// LittleFS guard (Contract 6, filesystem OTA). A recursive mutex serialises every
// read_file / write_file_atomic with the OTA remount (LittleFS.end/begin). While
// web_fs_offline() is set (a LittleFS image is being written, until the reboot),
// both fail fast, except for the task that holds the lock through
// fs_maintenance_begin() (the OTA finish task restoring the user's settings).
static SemaphoreHandle_t s_fs_mutex = NULL;
static const TickType_t FS_LOCK_WAIT = pdMS_TO_TICKS(3000);

void fs_lock_init()
{
    if (s_fs_mutex == NULL)
        s_fs_mutex = xSemaphoreCreateRecursiveMutex();
}

static bool fs_lock_is_mine()
{
    return s_fs_mutex != NULL &&
           xSemaphoreGetMutexHolder(s_fs_mutex) == xTaskGetCurrentTaskHandle();
}

// Takes the FS lock for a config read/write. False = do not touch LittleFS.
static bool fs_enter()
{
    if (!fs_mounted) return false;
    if (s_fs_mutex == NULL) return !web_fs_offline();   // before setup() created it
    bool maintainer = fs_lock_is_mine();
    if (web_fs_offline() && !maintainer) return false;
    if (xSemaphoreTakeRecursive(s_fs_mutex, FS_LOCK_WAIT) != pdTRUE) return false;
    if (web_fs_offline() && !maintainer) {     // went offline while we waited
        xSemaphoreGiveRecursive(s_fs_mutex);
        return false;
    }
    return true;
}

static void fs_leave()
{
    if (s_fs_mutex != NULL) xSemaphoreGiveRecursive(s_fs_mutex);
}

bool fs_maintenance_begin(uint32_t wait_ms)
{
    if (s_fs_mutex == NULL) return true;
    return xSemaphoreTakeRecursive(s_fs_mutex, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

void fs_maintenance_end()
{
    if (s_fs_mutex != NULL && fs_lock_is_mine())
        xSemaphoreGiveRecursive(s_fs_mutex);
}

bool read_file(const char* path, String& out)
{
    out = "";
    if (path == NULL || !fs_enter()) return false;
    bool ok = false;
    if (LittleFS.exists(path)) {
        File file = LittleFS.open(path, FILE_READ);
        if (file) {
            size_t size = file.size();
            if (size <= 4096) {                 // config files are tiny
                out.reserve(size + 1);
                while (file.available())
                    out += (char)file.read();
                ok = true;
            }
            file.close();
        }
    }
    fs_leave();
    if (!ok) out = "";
    return ok;
}

bool write_file_atomic(const char* path, const String& content)
{
    if (path == NULL || !fs_enter()) return false;
    bool ok = false;
    String tmp = String(path) + ".tmp";
    File file = LittleFS.open(tmp, FILE_WRITE);
    if (file) {
        size_t written = file.print(content);
        file.close();
        // LittleFS rename replaces an existing target atomically.
        if (written == content.length() && LittleFS.rename(tmp, path))
            ok = true;
        else
            LittleFS.remove(tmp);
    }
    fs_leave();
    return ok;
}

// ------------------------------------------------------------- dispatcher

static void send_username_to_ws()
{
    JsonDocument out;
    out["response_type"] = "set_uname";
    out["uname"] = get_username();
    String payload;
    serializeJson(out, payload);
    send_to_ws(payload);
}

static void send_serial_mode_to_ws()
{
    JsonDocument out;
    out["response_type"] = "serial_mode";
    out["value"] = (bool)lora_serial;
    String payload;
    serializeJson(out, payload);
    send_to_ws(payload);
}

static void send_bt_config_to_ws()
{
    BtConfig cfg = get_bt_config();
    JsonDocument out;
    out["response_type"] = "bt_config";
    out["bt_name"] = cfg.bt_name;
    out["bt_enabled"] = cfg.bt_enabled;
    out["bt_available"] = bt_is_available();   // true if VOYAGER_BT_ENABLED was compiled
    out["bt_running"] = bt_is_running();       // true if BluetoothSerial is currently active
    out["bt_connected"] = bt_is_connected();   // true if a BT client is connected
    String payload;
    serializeJson(out, payload);
    send_to_ws(payload);
}

// Task for reboot_to_bt: save config and restart. Every path ends in vTaskDelete.
static void reboot_to_bt_task(void* param)
{
    BtConfig cfg = get_bt_config();
    cfg.bt_enabled = true;
    if (save_bt_config(cfg)) {
        serial_print("BT enabled, rebooting...");
        show_alert("Bluetooth enabled, rebooting...");
    } else {
        serial_print("Failed to save BT config");
        show_alert("Failed to enable Bluetooth");
        vTaskDelete(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP.restart();
    vTaskDelete(NULL);
}

// Task for set_bt_mode: start/stop BT on demand (no reboot). Every path ends in vTaskDelete.
struct SetBtModeParam {
    bool enable;
};

static void set_bt_mode_task(void* param)
{
    SetBtModeParam* p = (SetBtModeParam*)param;
    bool enable = p->enable;
    delete p;

    bool success = false;
    if (enable) {
        // Start BT on demand (no reboot needed)
        success = start_bt_serial();
        if (success) {
            show_alert("Bluetooth started - pair with device now");
        } else {
            show_alert("Failed to start Bluetooth");
        }
    } else {
        // Stop BT to free resources
        success = stop_bt_serial();
        if (success) {
            show_alert("Bluetooth stopped");
        } else {
            show_alert("Bluetooth was not running");
        }
    }
    send_bt_config_to_ws();
    vTaskDelete(NULL);
}

// Send a file transfer invite broadcast over LoRa (LORA_MSG with pack_type=action)
static void send_file_tx_invite(const String& fileName, const String& fileSize)
{
    JsonDocument inner;
    inner["pack_type"] = "action";
    inner["data"] = "file_tx_invite";
    inner["sender_mac"] = WiFi.macAddress();
    inner["sender_name"] = get_username();
    inner["file_name"] = fileName;
    inner["file_size"] = fileSize;
    String inner_str;
    serializeJson(inner, inner_str);

    JsonDocument doc;
    doc["name"] = get_username();
    doc["mac"] = WiFi.macAddress();
    doc["data"] = inner_str;
    String payload;
    serializeJson(doc, payload);

    QueueParam* p = new (std::nothrow) QueueParam();
    if (!p) {
        show_alert("File invite failed: no memory");
        return;
    }
    p->type = LORA_MSG;
    p->message = payload;
    p->id = 0;   // no tx_result for broadcast
    if (send_packets == NULL || xQueueSend(send_packets, (void*)&p, pdMS_TO_TICKS(50)) != pdTRUE) {
        delete p;
        show_alert("File invite failed: queue full");
        return;
    }
    serial_print("File TX invite sent: " + fileName);
}

// Send a file transfer join/ack over LoRa (unicast LORA_MSG)
static void send_file_tx_join(const String& sender_mac, const String& mode)
{
    JsonDocument inner;
    inner["pack_type"] = "action";
    inner["data"] = "file_tx_ack";
    inner["receiver_mac"] = WiFi.macAddress();
    inner["receiver_name"] = get_username();
    inner["mode"] = mode;
    String inner_str;
    serializeJson(inner, inner_str);

    JsonDocument doc;
    doc["name"] = get_username();
    doc["mac"] = WiFi.macAddress();
    doc["data"] = inner_str;
    String payload;
    serializeJson(doc, payload);

    QueueParam* p = new (std::nothrow) QueueParam();
    if (!p) {
        show_alert("File join failed: no memory");
        return;
    }
    p->type = LORA_MSG;
    p->message = payload;
    p->id = 0;
    if (send_packets == NULL || xQueueSend(send_packets, (void*)&p, pdMS_TO_TICKS(50)) != pdTRUE) {
        delete p;
        show_alert("File join failed: queue full");
        return;
    }
    serial_print("File TX join sent to " + sender_mac + " mode=" + mode);
}

// WS command dispatcher (Contract 3). Runs on the web_sockets worker task.
// Validation lives in core_dispatch.h (unit-tested natively); this only routes.
// Never logs argument values for wifi / ap passwords.
void handle_operations(JsonDocument& doc)
{
    CoreCommand cmd = core_parse_request(doc.as<JsonVariantConst>());
    switch (cmd.action)
    {
    case CoreAction::Missing:
    case CoreAction::Unknown:
        serial_print(String("WS request ignored: ") + cmd.error);
        return;
    case CoreAction::Invalid:
        serial_print(String("WS request rejected: ") + cmd.error);
        show_alert(String("Request rejected: ") + cmd.error);
        return;
    case CoreAction::WifiScan:
        if (xTaskCreatePinnedToCore(scan_ssid, "scan_wifi", 6000, NULL, 1, NULL, 1) != pdPASS)
            serial_print("scan_wifi task create failed");
        return;
    case CoreAction::ConnectWifi:
        serial_print("connect_wifi: " + String(cmd.arg1.c_str()));
        wifi_save_sta(cmd.arg1.c_str(), cmd.arg2.c_str());   // alerts on failure itself
        return;
    case CoreAction::WifiApMode:
        serial_print("wifi_ap_mode");
        wifi_switch_to_ap();   // alerts on failure itself
        return;
    case CoreAction::SetApPass:
        serial_print("set_ap_pass");
        wifi_set_ap_pass(cmd.arg1.c_str());   // alerts on failure itself
        return;
    case CoreAction::ResetDevice:
        if (xTaskCreatePinnedToCore(reset_device, "reset_device", 6000, NULL, 1, NULL, 1) != pdPASS)
            serial_print("reset_device task create failed");
        return;
    case CoreAction::RestartDevice:
        if (xTaskCreatePinnedToCore(restart, "restart", 4096, NULL, 1, NULL, 1) != pdPASS)
            serial_print("restart task create failed");
        return;
    case CoreAction::SetUsername:
        if (save_username(String(cmd.arg1.c_str())))
            show_alert("Username saved");
        else
            show_alert("Username could not be saved");
        send_username_to_ws();
        return;
    case CoreAction::GetUsername:
        send_username_to_ws();
        return;
    case CoreAction::SetLoraConfig:
        serial_print("set_lora_config");
        if (!save_lora_config(cmd.arg1.c_str()))
            show_alert("LoRa config rejected");
        return;
    case CoreAction::SetSerialMode:
        // The serial bridge and sensor broadcast mode are mutually exclusive.
        if (cmd.flag && sensor_mode_enabled())
        {
            show_alert("Turn off sensor broadcast before enabling LoRa to Serial");
            send_serial_mode_to_ws();
            return;
        }
        if (cmd.flag && net_active()) {
            show_alert("The serial port is in LoRa network mode (Voyager Link).");
            send_serial_mode_to_ws();
            return;
        }
        lora_serial = cmd.flag;
        if (xTaskCreatePinnedToCore(save_lora_serial_config, "save_lora_serial", 4096, NULL, 1, NULL, 1) != pdPASS)
            serial_print("save_lora_serial task create failed");
        send_serial_mode_to_ws();
        return;
    case CoreAction::GetSerialMode:
        send_serial_mode_to_ws();
        return;
    case CoreAction::GetNetStatus:
    {
        // net_status_json() is a complete object; tag it for the UI.
        String s = net_status_json();
        send_to_ws("{\"response_type\":\"net_status\"," + s.substring(1));
        return;
    }
    case CoreAction::GetWifiScan:
    {
        // Last complete scan only; never starts a scan. Nothing sent if none yet.
        String scan = wifi_last_scan_json();
        if (scan.length() > 0)
            send_to_ws(scan);
        return;
    }
    // Bluetooth Serial commands
    case CoreAction::GetBtConfig:
        send_bt_config_to_ws();
        return;
    case CoreAction::SetBtMode:
    {
        if (cmd.flag && !bt_is_available()) {
            show_alert("BT not available (firmware not compiled with VOYAGER_BT_ENABLED)");
            send_bt_config_to_ws();
            return;
        }
        SetBtModeParam* p = new (std::nothrow) SetBtModeParam();
        if (!p) {
            show_alert("Failed: out of memory");
            return;
        }
        p->enable = cmd.flag;
        if (xTaskCreatePinnedToCore(set_bt_mode_task, "set_bt_mode", 4096, p, 1, NULL, 1) != pdPASS) {
            delete p;
            serial_print("set_bt_mode task create failed");
        }
        return;
    }
    case CoreAction::RebootToBt:
        if (!bt_is_available()) {
            show_alert("BT not available (firmware not compiled with VOYAGER_BT_ENABLED)");
            send_bt_config_to_ws();
            return;
        }
        if (xTaskCreatePinnedToCore(reboot_to_bt_task, "reboot_to_bt", 4096, NULL, 1, NULL, 1) != pdPASS)
            serial_print("reboot_to_bt task create failed");
        return;
    // File transfer handshake commands
    case CoreAction::FileTxInvite:
        if (sensor_mode_enabled()) {
            show_alert("Sensor broadcast is on. Turn it off to transfer files.");
            return;
        }
        send_file_tx_invite(String(cmd.arg1.c_str()), String(cmd.arg2.c_str()));
        return;
    case CoreAction::FileTxJoin:
        if (sensor_mode_enabled()) {
            show_alert("Sensor broadcast is on. Turn it off to transfer files.");
            return;
        }
        send_file_tx_join(String(cmd.arg1.c_str()), String(cmd.arg2.c_str()));
        return;
    case CoreAction::FileTxStart:
        if (sensor_mode_enabled()) {
            show_alert("Sensor broadcast is on. Turn it off to transfer files.");
            send_serial_mode_to_ws();
            return;
        }
        if (net_active()) {
            show_alert("The serial port is in LoRa network mode (Voyager Link). Leave the network first.");
            send_serial_mode_to_ws();
            return;
        }
        lora_serial = true;
        serial_print("File TX started (serial bridge enabled)");
        send_serial_mode_to_ws();
        return;
    case CoreAction::FileTxEnd:
        lora_serial = false;
        serial_print("File TX ended (serial bridge disabled)");
        send_serial_mode_to_ws();
        return;
    case CoreAction::SetSensorConfig:
        serial_print("set_sensor_config");
        if (sensor_save_config(cmd.arg1.c_str()))   // alerts on failure itself
        {
            // Enabling sensor mode switches the serial bridge off.
            if (sensor_mode_enabled() && lora_serial)
            {
                lora_serial = false;
                if (xTaskCreatePinnedToCore(save_lora_serial_config, "save_lora_serial", 4096, NULL, 1, NULL, 1) != pdPASS)
                    serial_print("save_lora_serial task create failed");
                send_serial_mode_to_ws();
            }
            show_alert("Sensor settings saved");
        }
        sensor_send_config_to_ws();
        return;
    case CoreAction::GetSensorConfig:
        sensor_send_config_to_ws();
        return;
    }
}

// Saves the current lora_serial flag. Task: every path ends in vTaskDelete.
void save_lora_serial_config(void* param)
{
    JsonDocument doc;
    doc["lora_serial"] = (bool)lora_serial;
    String config;
    serializeJson(doc, config);
    if (write_file_atomic("/config/lora_serial.json", config))
        serial_print("LoRa serial config saved.");
    else
        serial_print("LoRa serial config save failed.");
    vTaskDelete(NULL);
}

// ------------------------------------------------------ LoRa <-> serial bridge

// Received LORA_SERIAL payloads -> active serial (USB or BT). Runs in every build (not DEBUG-gated).
void send_to_serial(void *param)
{
    while (true)
    {
        DebugQueueParam *p = NULL;
        if (xQueueReceive(serial_packet_rec, &p, portMAX_DELAY) == pdTRUE && p)
        {
            // Only devices that joined a transfer (bridge on) forward LORA_SERIAL bytes;
            // otherwise a bystander's console would fill with another link's YMODEM traffic.
            if (lora_serial)
                active_serial_write((const uint8_t*)p->message.c_str(), p->message.length());
            delete p;
        }
    }
}

void send_to_lora(void *param)
{
    while (true)
    {
        DebugQueueParam *p = NULL;
        if (xQueueReceive(serial_packet_send, &p, portMAX_DELAY) == pdTRUE && p)
        {
            // Snapshot the flag once per message to avoid partial-send if toggled mid-packet.
            bool enabled = lora_serial;
            if (enabled)
                // seq 0, not a fresh one: LORA_SERIAL is never ACKed or deduplicated, and
                // taking seqs here would let a later LORA_MSG land exactly 256 seqs after
                // the previous one and be dropped by the receiver's dedup as a "duplicate".
                LoRa_send(p->message, LORA_SERIAL, 0);
            delete p;
        }
    }
}

// A frame also ends when the line goes quiet this long (end of a burst that has no CR).
#define SERIAL_FRAME_IDLE_MS 10

// Active serial (USB or BT) -> LoRa. A LoRa frame ends at CR, at LORA_MAX_PAYLOAD
// bytes, or after SERIAL_FRAME_IDLE_MS of silence. Kermit packets never contain a raw
// CR except their terminator, so each packet (<= 251 bytes) travels as exactly one
// frame and a lost frame costs one packet. Terminal text gets one frame per line.
void serial_to_lora(void* param)
{
    while (true)
    {
        // Snapshot the flag once per iteration to avoid inconsistent behavior mid-loop.
        bool enabled = lora_serial;
        if (enabled)
        {
            if (active_serial_available() > 0)
            {
                DebugQueueParam *p = new (std::nothrow) DebugQueueParam();
                if (p)
                {
                    p->message.reserve(LORA_MAX_PAYLOAD);
                    uint32_t last = millis();
                    while (p->message.length() < LORA_MAX_PAYLOAD)
                    {
                        if (active_serial_available() > 0)
                        {
                            char c = (char)active_serial_read();
                            p->message += c;
                            last = millis();
                            if (c == '\r') break;
                        }
                        else if (millis() - last >= SERIAL_FRAME_IDLE_MS)
                            break;
                        else
                            vTaskDelay(1);
                    }
                    if (xQueueSend(serial_packet_send, (void*)&p, pdMS_TO_TICKS(100)) != pdTRUE)
                        delete p;
                }
                continue;   // next frame immediately
            }
        }
        else
        {
            // Bridge off: USB input is the LoRa network (SLIP) when Voyager Link turned
            // it on, else Improv (WiFi setup / network start); anything else is dropped
            // by the respective parser.
            if (net_active()) {
                while (Serial.available()) net_feed((uint8_t)Serial.read());
                net_poll();
            } else {
                while (Serial.available()) improv_feed((uint8_t)Serial.read());
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Must run after config_lora() (radio mutex) and after the queues exist.
// Halts on failure: the serial bridge is core functionality (Contract 6).
void start_serial_bridge()
{
    net_init();   // LoRa network mode (USB SLIP), entered via Improv vendor command
    if (xTaskCreatePinnedToCore(send_to_serial, "send_to_serial", 4096, NULL, 1, NULL, 1) != pdPASS)
        halt_boot("serial rx task");
    if (xTaskCreatePinnedToCore(send_to_lora, "send_to_lora", 6000, NULL, 1, NULL, 1) != pdPASS)
        halt_boot("serial tx task");
    if (xTaskCreatePinnedToCore(serial_to_lora, "serial_to_lora", 4096, NULL, 1, NULL, 1) != pdPASS)
        halt_boot("serial in task");
}

void get_lora_serial()
{
    String cfg;
    bool ok = read_file("/config/lora_serial.json", cfg);
    lora_serial = core_parse_lora_serial(ok ? cfg.c_str() : nullptr);
}

// ---------------------------------------------------------------- username

void load_username()
{
    String cfg;
    bool ok = read_file("/config/user_data.json", cfg);
    std::string mac = WiFi.macAddress().c_str();
    std::string u = core_parse_username(ok ? cfg.c_str() : nullptr, mac);
    std::lock_guard<std::mutex> g(s_username_lock);
    s_username = u.c_str();
}

String get_username()
{
    std::lock_guard<std::mutex> g(s_username_lock);
    if (s_username.length() == 0)
        return WiFi.macAddress();
    return s_username;   // copy made under the lock
}

// Caller has validated length (1-32). Saved atomically; memory updated only on success.
bool save_username(const String& uname)
{
    JsonDocument doc;
    doc["username"] = uname;
    String cfg;
    serializeJson(doc, cfg);
    if (!write_file_atomic("/config/user_data.json", cfg))
    {
        serial_print("Username save failed");
        return false;
    }
    {
        std::lock_guard<std::mutex> g(s_username_lock);
        s_username = uname;
    }
    serial_print("Username saved");
    return true;
}

// ------------------------------------------------------------ WS helpers


void show_alert(String msg)
{
    JsonDocument doc;
    doc["response_type"] = "alert";
    doc["alert_msg"] = msg;
    String payload;
    serializeJson(doc, payload);
    send_to_ws(payload);
}

// --------------------------------------------------------- restart / reset

// Task. Short delay lets the WS/HTTP reply flush first.
void restart(void *param)
{
    serial_print("Restarting esp.");
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP.restart();
    vTaskDelete(NULL);   // not reached
}

static bool restore_default(const char* target, const char* source)
{
    String content;
    if (!read_file(source, content) || content.length() == 0)
        return false;
    return write_file_atomic(target, content);
}

// Factory reset (Contract 5). Task. Does NOT format: the web UI stays.
void reset_device(void *param)
{
    serial_print("Factory reset");
    show_alert("Device reset successfully,\nPlease reconfigure the device by connecting to the AP.\nRebooting Now.");
    oled_alert("Resetting", "Restoring defaults", 5000);

    bool ok = true;
    if (!restore_default("/config/wifi_config.json", "/config/wifi_default.json"))
    {
        // No default file: write the built-in AP default (empty ap_pass = derived default).
        ok &= write_file_atomic("/config/wifi_config.json",
            "{\"wifi_function\":\"AP\",\"wifi_ssid\":\"\",\"wifi_pass\":\"\",\"ap_pass\":\"\"}");
    }
    if (!restore_default("/config/lora_config.json", "/config/lora_default.json"))
    {
        // No default: remove the active config so the radio uses its compiled defaults.
        if (fs_maintenance_begin(3000)) {
            if (fs_mounted && !web_fs_offline() && LittleFS.exists("/config/lora_config.json"))
                ok &= LittleFS.remove("/config/lora_config.json");
            fs_maintenance_end();
        }
    }
    ok &= write_file_atomic("/config/user_data.json", "{\"username\":\"\"}");
    ok &= write_file_atomic("/config/lora_serial.json", "{\"lora_serial\":false}");
    ok &= write_file_atomic(SENSOR_CONFIG_PATH,
        "{\"enabled\":false,\"interval\":30,\"ack\":false,\"sensors\":[]}");
    lora_serial = false;
    auth_clear();
    if (!ok)
        serial_print("Factory reset: some config files could not be restored");

    vTaskDelay(pdMS_TO_TICKS(1500));   // let the alert reach the browser / OLED
    ESP.restart();
    vTaskDelete(NULL);   // not reached
}

// ---------------------------------------------------------------- GPIO

static void config_button_pin(int pin)
{
    // GPIO 34-39 are input-only with no internal pulls: external resistor required.
    if (pin >= 34 && pin <= 39)
        pinMode(pin, INPUT);
    else
        pinMode(pin, STATE == HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);
}

void config_gpios()
{
    pinMode(LED, OUTPUT);
    digitalWrite(LED, LOW);
    config_button_pin(BTN1);
    config_button_pin(BTN2);
}

// ---------------------------------------------------------------- logging

// Lines lost because the queue was full or out of memory. Reported (and reset)
// by the logger task so silent gaps in the log are visible.
static volatile uint32_t s_debug_dropped = 0;
static portMUX_TYPE s_debug_mux = portMUX_INITIALIZER_UNLOCKED;

static void debug_count_drop()
{
    portENTER_CRITICAL(&s_debug_mux);
    s_debug_dropped = s_debug_dropped + 1;
    portEXIT_CRITICAL(&s_debug_mux);
}

// Queue a debug line. No-op in release builds, from ISRs, or before the
// logger exists. Callers must never pass secrets (Contract 4).
void serial_print(String msg)
{
    if (!DEBUGGING) return;
    if (xPortInIsrContext()) return;
    if (debug_handler == NULL || debug_msg == NULL) return;
    DebugQueueParam *payload = new (std::nothrow) DebugQueueParam();
    if (!payload) { debug_count_drop(); return; }
    payload->message = msg;
    if (xQueueSend(debug_msg, (void*)&payload, 0) != pdTRUE)
    {
        delete payload;   // queue full: drop rather than block the caller
        debug_count_drop();
    }
}

void debugger_print(void *param)
{
    while (true)
    {
        DebugQueueParam* params = NULL;
        if (xQueueReceive(debug_msg, &params, portMAX_DELAY) != pdTRUE || !params)
            continue;
        portENTER_CRITICAL(&s_debug_mux);
        uint32_t dropped = s_debug_dropped;
        s_debug_dropped = 0;
        portEXIT_CRITICAL(&s_debug_mux);
        if (dropped > 0)
        {
            String note = "(" + String(dropped) + " debug lines dropped)";
            if (DEBUGGING && !lora_serial && !net_active())
                Serial.println(note);
        }
        if (DEBUGGING && !lora_serial && !net_active())   // the UART belongs to the bridge / network
            Serial.println(params->message);
        if (DEBUGGING && events_client_count() > 0)
        {
            JsonDocument doc;
            doc["millis"] = millis();
            doc["data"] = params->message;
            String payload;
            serializeJson(doc, payload);
            send_to_events(payload, "DEBUG");
        }
        delete params;
    }
}

// Reserved for future device modes (e.g., BLE, LoRa-only). Currently unused at boot.
String get_device_mode()
{
    String cfg;
    bool ok = read_file("/config/device_config.json", cfg);
    return String(core_parse_device_mode(ok ? cfg.c_str() : nullptr).c_str());
}

void setupTasks()
{
    // LED task first so led_nortifier() has a target as early as possible.
    // Non-critical: failure just means no LED blink feedback.
    if (xTaskCreatePinnedToCore(async_led_notifier, "led_notifier", 2048, NULL, 1, &led_task_handle, 1) != pdPASS)
        serial_print("led task create failed");

    // Button task is critical: provides SOS and OLED navigation.
    if (xTaskCreatePinnedToCore(btn_intrupt, "buttons", 4096, NULL, 2, NULL, 1) != pdPASS)
        halt_boot("button task");

    // Telemetry tasks are non-critical: failure just disables SSE updates.
    if (xTaskCreatePinnedToCore(get_heap_info, "get_heap_info", 4096, NULL, 1, NULL, 1) != pdPASS)
        serial_print("heap task create failed");
    if (xTaskCreatePinnedToCore(get_wifi_rssi, "get_wifi_rssi", 4096, NULL, 1, NULL, 1) != pdPASS)
        serial_print("rssi task create failed");
}

// ------------------------------------- wifi-network-engineer owned functions
// Owner: wifi-network-engineer. Starts mDNS once per boot; the IDF responder
// follows AP/STA interfaces by itself, so reconnects need no new begin().
// Precondition: wifi_init_hostname() has already set hostname.
void setup_mdns()
{
    static bool mdns_started = false;
    if (mdns_started) return;
    if (MDNS.begin(hostname.c_str())) {
        MDNS.addService("http", "tcp", 80);
        mdns_started = true;
        serial_print("Started mDNS service: " + hostname);
    } else {
        serial_print("mDNS start failed");
    }
}

// Owner: wifi-network-engineer. Captive DNS for the AP; started once per boot.
void setup_dns()
{
    static bool dns_started = false;
    if (dns_started) return;
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.setTTL(3600);
    if (!dnsServer.start(53, "*", WiFi.softAPIP()))
    {
        serial_print("DNS server start failed");
        return;
    }
    if (xTaskCreatePinnedToCore(dns_request_process, "DNS Request Handler", 4096, NULL, 1, NULL, 1) != pdPASS)
    {
        dnsServer.stop();
        serial_print("DNS task create failed");
        return;
    }
    dns_started = true;
    serial_print("DNS service started on " + WiFi.softAPIP().toString());
}

// Owner: wifi-network-engineer. Never returns.
void dns_request_process(void *parameter)
{
    for(;;)
    {
        dnsServer.processNextRequest();
        vTaskDelay(30/portTICK_PERIOD_MS);
    }
}
