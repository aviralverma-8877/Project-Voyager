#include "support_method.h"
#include "core_dispatch.h"
#include <web/web_sockets.h>
#include <web/auth.h>
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

bool read_file(const char* path, String& out)
{
    out = "";
    if (!fs_mounted || !LittleFS.exists(path)) return false;
    File file = LittleFS.open(path, FILE_READ);
    if (!file) return false;
    size_t size = file.size();
    if (size > 4096) { file.close(); return false; }   // config files are tiny
    out.reserve(size + 1);
    while (file.available())
        out += (char)file.read();
    file.close();
    return true;
}

bool write_file_atomic(const char* path, const String& content)
{
    if (!fs_mounted || path == NULL) return false;
    String tmp = String(path) + ".tmp";
    File file = LittleFS.open(tmp, FILE_WRITE);
    if (!file) return false;
    size_t written = file.print(content);
    file.close();
    if (written != content.length()) {
        LittleFS.remove(tmp);
        return false;
    }
    // LittleFS rename replaces an existing target atomically.
    if (!LittleFS.rename(tmp, path)) {
        LittleFS.remove(tmp);
        return false;
    }
    return true;
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
        lora_serial = cmd.flag;
        if (xTaskCreatePinnedToCore(save_lora_serial_config, "save_lora_serial", 4096, NULL, 1, NULL, 1) != pdPASS)
            serial_print("save_lora_serial task create failed");
        send_serial_mode_to_ws();
        return;
    case CoreAction::GetSerialMode:
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

// Received LORA_SERIAL payloads -> UART. Runs in every build (not DEBUG-gated).
void send_to_serial(void *param)
{
    while (true)
    {
        DebugQueueParam *p = NULL;
        if (xQueueReceive(serial_packet_rec, &p, portMAX_DELAY) == pdTRUE && p)
        {
            Serial.print(p->message);
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
                LoRa_send(p->message, LORA_SERIAL);
            delete p;
        }
    }
}

// UART -> LoRa. Each chunk is at most LORA_MAX_PAYLOAD bytes.
void serial_to_lora(void* param)
{
    while (true)
    {
        // Snapshot the flag once per iteration to avoid inconsistent behavior mid-loop.
        bool enabled = lora_serial;
        if (enabled)
        {
            if (Serial.available())
            {
                // Give a short burst time to arrive so we send fuller chunks.
                vTaskDelay(pdMS_TO_TICKS(20));
                DebugQueueParam *p = new (std::nothrow) DebugQueueParam();
                if (p)
                {
                    p->message.reserve(LORA_MAX_PAYLOAD);
                    while (Serial.available() && p->message.length() < LORA_MAX_PAYLOAD)
                        p->message += (char)Serial.read();
                    if (xQueueSend(serial_packet_send, (void*)&p, pdMS_TO_TICKS(100)) != pdTRUE)
                        delete p;
                }
                continue;   // drain any remainder immediately
            }
        }
        else
        {
            while (Serial.available()) Serial.read();   // discard console input
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Must run after config_lora() (radio mutex) and after the queues exist.
// Halts on failure: the serial bridge is core functionality (Contract 6).
void start_serial_bridge()
{
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

void stop_transmission()
{
    send_to_ws("{\"response_type\":\"stop_transmission\"}");
}

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
        if (fs_mounted && LittleFS.exists("/config/lora_config.json"))
            ok &= LittleFS.remove("/config/lora_config.json");
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

// Queue a debug line. No-op in release builds, from ISRs, or before the
// logger exists. Callers must never pass secrets (Contract 4).
void serial_print(String msg)
{
    if (!DEBUGGING) return;
    if (xPortInIsrContext()) return;
    if (debug_handler == NULL || debug_msg == NULL) return;
    DebugQueueParam *payload = new (std::nothrow) DebugQueueParam();
    if (!payload) return;
    payload->message = msg;
    if (xQueueSend(debug_msg, (void*)&payload, 0) != pdTRUE)
        delete payload;   // queue full: drop rather than block the caller
}

void debugger_print(void *param)
{
    while (true)
    {
        DebugQueueParam* params = NULL;
        if (xQueueReceive(debug_msg, &params, portMAX_DELAY) != pdTRUE || !params)
            continue;
        if (DEBUGGING && !lora_serial)   // the UART belongs to the bridge in serial mode
        {
            Serial.print("\n");
            Serial.println(params->message);
        }
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
