#include "sensor_support.h"
#include "sensor_config.h"
#include <core/support_method.h>
#include <lora/lora_support.h>
#include <web/web_server.h>
#include <web/web_sockets.h>
#include <atomic>
#include <mutex>
#include <new>

using namespace sensor_cfg;

// This board's own pins (platformio.ini custom_pins): never offered to sensors.
static const int8_t USED_PINS[] = {
    DIO0, DIO1,
#ifdef DIO2
    DIO2,
#endif
    LMOSI, LMISO, LNSS, LSCK, LRST, BTN1, BTN2, LED, OSCL, OSDA};
static const size_t USED_PINS_N = sizeof(USED_PINS) / sizeof(USED_PINS[0]);

static Config            s_cfg;                 // guarded by s_cfg_lock
static std::mutex        s_cfg_lock;
static std::atomic<bool> s_enabled{false};
static TaskHandle_t      s_task = NULL;

// Bit-banged protocols (DHT, 1-Wire) need microsecond timing: interrupts are
// off on this core for at most ~5 ms per read.
static portMUX_TYPE s_timing_mux = portMUX_INITIALIZER_UNLOCKED;

static Config config_snapshot()
{
    std::lock_guard<std::mutex> g(s_cfg_lock);
    return s_cfg;
}

bool sensor_mode_enabled()
{
    return s_enabled.load();
}

// ------------------------------------------------------------------ drivers

// Waits while the pin stays at `level`. False on timeout; *dur_us = how long it stayed.
static bool wait_while(int pin, int level, uint32_t timeout_us, uint32_t* dur_us)
{
    int64_t start = esp_timer_get_time();
    while (digitalRead(pin) == level) {
        if ((uint32_t)(esp_timer_get_time() - start) > timeout_us) return false;
    }
    if (dur_us) *dur_us = (uint32_t)(esp_timer_get_time() - start);
    return true;
}

// DHT11 / DHT22 single-wire read. v[0] = temperature C, v[1] = humidity %.
static bool read_dht(int pin, bool dht22, float* v)
{
    uint8_t d[5] = {0, 0, 0, 0, 0};
    // Open drain + pull-up: HIGH releases the line, and the pin can still be read.
    pinMode(pin, OUTPUT_OPEN_DRAIN | PULLUP);
    digitalWrite(pin, HIGH);
    vTaskDelay(pdMS_TO_TICKS(2));
    digitalWrite(pin, LOW);                 // start signal (DHT11 needs >= 18 ms)
    vTaskDelay(pdMS_TO_TICKS(20));

    bool ok = true;
    portENTER_CRITICAL(&s_timing_mux);
    digitalWrite(pin, HIGH);                // release; sensor answers after 20-40 us
    delayMicroseconds(10);
    ok = wait_while(pin, HIGH, 200, NULL)   // sensor pulls low (80 us)
      && wait_while(pin, LOW, 200, NULL)    // then high (80 us)
      && wait_while(pin, HIGH, 200, NULL);  // first bit starts
    for (int i = 0; ok && i < 40; i++) {
        uint32_t lo = 0, hi = 0;
        ok = wait_while(pin, LOW, 150, &lo) && wait_while(pin, HIGH, 150, &hi);
        // 50 us low, then 26-28 us high = 0, 70 us high = 1.
        if (ok && hi > lo) d[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    }
    portEXIT_CRITICAL(&s_timing_mux);

    if (!ok) return false;
    if ((uint8_t)(d[0] + d[1] + d[2] + d[3]) != d[4]) return false;
    if (dht22) {
        v[1] = (float)(((uint16_t)d[0] << 8) | d[1]) * 0.1f;
        float t = (float)((((uint16_t)d[2] & 0x7F) << 8) | d[3]) * 0.1f;
        v[0] = (d[2] & 0x80) ? -t : t;
    } else {
        v[1] = (float)d[0] + (float)d[1] * 0.1f;
        float t = (float)d[2] + (float)(d[3] & 0x0F) * 0.1f;
        v[0] = (d[3] & 0x80) ? -t : t;
    }
    return v[1] >= 0.0f && v[1] <= 100.0f && v[0] >= -40.0f && v[0] <= 125.0f;
}

// ---- 1-Wire (one DS18B20 per pin, addressed with Skip ROM) ----

static bool ow_reset(int pin)
{
    portENTER_CRITICAL(&s_timing_mux);
    digitalWrite(pin, LOW);
    delayMicroseconds(480);
    digitalWrite(pin, HIGH);
    delayMicroseconds(70);
    bool present = digitalRead(pin) == LOW;
    portEXIT_CRITICAL(&s_timing_mux);
    delayMicroseconds(410);
    return present;
}

static void ow_write_byte(int pin, uint8_t b)
{
    for (int i = 0; i < 8; i++, b >>= 1) {
        portENTER_CRITICAL(&s_timing_mux);
        digitalWrite(pin, LOW);
        if (b & 1) {
            delayMicroseconds(6);
            digitalWrite(pin, HIGH);
            delayMicroseconds(64);
        } else {
            delayMicroseconds(60);
            digitalWrite(pin, HIGH);
            delayMicroseconds(10);
        }
        portEXIT_CRITICAL(&s_timing_mux);
    }
}

static uint8_t ow_read_byte(int pin)
{
    uint8_t b = 0;
    for (int i = 0; i < 8; i++) {
        portENTER_CRITICAL(&s_timing_mux);
        digitalWrite(pin, LOW);
        delayMicroseconds(3);
        digitalWrite(pin, HIGH);
        delayMicroseconds(10);
        if (digitalRead(pin)) b |= (uint8_t)(1 << i);
        portEXIT_CRITICAL(&s_timing_mux);
        delayMicroseconds(53);
    }
    return b;
}

static uint8_t ow_crc8(const uint8_t* data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t in = data[i];
        for (int b = 0; b < 8; b++) {
            uint8_t mix = (crc ^ in) & 0x01;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            in >>= 1;
        }
    }
    return crc;
}

// v[0] = temperature C. Takes ~800 ms (12-bit conversion).
static bool read_ds18b20(int pin, float* v)
{
    pinMode(pin, OUTPUT_OPEN_DRAIN | PULLUP);
    digitalWrite(pin, HIGH);
    if (!ow_reset(pin)) return false;
    ow_write_byte(pin, 0xCC);               // Skip ROM
    ow_write_byte(pin, 0x44);               // Convert T
    vTaskDelay(pdMS_TO_TICKS(800));
    if (!ow_reset(pin)) return false;
    ow_write_byte(pin, 0xCC);
    ow_write_byte(pin, 0xBE);               // Read scratchpad
    uint8_t s[9];
    bool all_zero = true;
    for (int i = 0; i < 9; i++) {
        s[i] = ow_read_byte(pin);
        if (s[i] != 0) all_zero = false;
    }
    if (all_zero || ow_crc8(s, 8) != s[8]) return false;   // all zero = line stuck low
    int16_t raw = (int16_t)(((uint16_t)s[1] << 8) | s[0]);
    v[0] = (float)raw / 16.0f;
    return v[0] >= -55.0f && v[0] <= 125.0f;
}

// v[0] = distance in cm (2-400).
static bool read_hcsr04(int trig, int echo, float* v)
{
    pinMode(trig, OUTPUT);
    pinMode(echo, INPUT);
    digitalWrite(trig, LOW);
    delayMicroseconds(4);
    digitalWrite(trig, HIGH);
    delayMicroseconds(10);
    digitalWrite(trig, LOW);
    unsigned long us = pulseIn(echo, HIGH, 30000UL);
    if (us == 0) return false;
    v[0] = (float)us / 58.0f;
    return v[0] >= 2.0f && v[0] <= 400.0f;
}

// v[0] = raw ADC1 value 0-4095, averaged.
static bool read_analog(int pin, float* v)
{
    pinMode(pin, INPUT);
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) sum += analogRead(pin);
    v[0] = (float)(sum / 8);
    return true;
}

// v[0] = 0 / 1. GPIO 34-39 have no internal pull resistors.
static bool read_digital(int pin, float* v)
{
    pinMode(pin, pin_is_input_only(pin) ? INPUT : INPUT_PULLUP);
    v[0] = digitalRead(pin) ? 1.0f : 0.0f;
    return true;
}

static void read_sensor(const Sensor& s, Reading& r)
{
    strcpy(r.name, s.name);
    r.type = s.type;
    r.v[0] = r.v[1] = 0;
    switch (s.type) {
        case T_DHT11:   r.ok = read_dht(s.pin, false, r.v); break;
        case T_DHT22:   r.ok = read_dht(s.pin, true, r.v); break;
        case T_DS18B20: r.ok = read_ds18b20(s.pin, r.v); break;
        case T_HCSR04:  r.ok = read_hcsr04(s.pin, s.pin2, r.v); break;
        case T_ANALOG:  r.ok = read_analog(s.pin, r.v); break;
        case T_DIGITAL: r.ok = read_digital(s.pin, r.v); break;
        default:        r.ok = false; break;
    }
    // One retry: DHT and 1-Wire reads fail now and then when an interrupt lands badly.
    if (!r.ok && (s.type == T_DHT11 || s.type == T_DHT22)) {
        vTaskDelay(pdMS_TO_TICKS(s.type == T_DHT22 ? 2100 : 1100));
        r.ok = read_dht(s.pin, s.type == T_DHT22, r.v);
    }
}

// ------------------------------------------------------------- broadcasting

// Local copy for the device's own web UI (WS `sensor_data`).
static void send_readings_to_ws(const Reading* r, size_t n)
{
    // ArduinoJson keeps a const char* by pointer, so the string must outlive
    // serializeJson (a temporary here used to send garbage to the page).
    const std::string inner = inner_json(r, n);
    JsonDocument doc;
    doc["response_type"] = "sensor_data";
    doc["data"] = inner.c_str();
    String out;
    serializeJson(doc, out);
    send_to_ws(out);
}

static void broadcast(const Reading* r, size_t n, bool ack)
{
    if (send_packets == NULL) return;
    // Never pile sensor packets up behind a slow link: skip this round instead.
    if (uxQueueMessagesWaiting(send_packets) >= uxQueueSpacesAvailable(send_packets)) {   // half full
        serial_print("Sensor broadcast skipped: send queue busy");
        return;
    }
    String name = get_username();
    String mac = WiFi.macAddress();
    size_t start = 0;
    while (start < n) {
        std::string inner;
        size_t taken = pack(r, n, start, name.c_str(), mac.c_str(), LORA_MAX_PAYLOAD, inner);
        if (taken == 0) {
            serial_print("Sensor reading does not fit in one packet, skipped");
            start++;
            continue;
        }
        start += taken;

        QueueParam* q = new (std::nothrow) QueueParam();
        if (q == NULL) return;
        {
            JsonDocument doc;
            doc["name"] = name;
            doc["mac"] = mac;
            doc["data"] = inner.c_str();
            serializeJson(doc, q->message);
        }
        q->type = LORA_MSG;
        q->id = 0;              // device-originated: no tx_result
        q->no_ack = !ack;
        q->quiet = true;        // a missed reading is not worth an alert
        if (xQueueSend(send_packets, &q, 0) != pdTRUE) {
            delete q;
            return;
        }
    }
}

// Task notification bits.
static const uint32_t NOTIFY_CONFIG = 0x01;   // config changed: start over with the new one
static const uint32_t NOTIFY_READ   = 0x02;   // "Read now" from the web page

static void read_all(const Config& c, Reading* readings)
{
    for (uint8_t i = 0; i < c.count; i++) {
        read_sensor(c.sensors[i], readings[i]);
        if (!readings[i].ok)
            serial_print(String("Sensor read failed: ") + c.sensors[i].name);
    }
}

// One-off read for the web page only (nothing goes on the air). Works with the
// mode off, so wiring can be checked before enabling the broadcast.
static void read_for_page()
{
    Config c = config_snapshot();
    Reading readings[MAX_COUNT];
    read_all(c, readings);
    send_readings_to_ws(readings, c.count);
}

static void sensor_task(void* param)
{
    for (;;) {
        Config c = config_snapshot();
        uint32_t bits = 0;
        if (!c.enabled || c.count == 0) {
            xTaskNotifyWait(0, 0xFFFFFFFF, &bits, portMAX_DELAY);
            if (bits & NOTIFY_READ) read_for_page();
            continue;
        }
        TickType_t started = xTaskGetTickCount();

        Reading readings[MAX_COUNT];
        read_all(c, readings);
        // The config may have been switched off during a slow read.
        if (sensor_mode_enabled()) {
            send_readings_to_ws(readings, c.count);
            broadcast(readings, c.count, c.ack);
        }

        // Sleep until the next broadcast. "Read now" is served in between without
        // moving the schedule; a config change starts over at once.
        const TickType_t period = pdMS_TO_TICKS((uint32_t)c.interval * 1000UL);
        for (;;) {
            TickType_t elapsed = xTaskGetTickCount() - started;
            if (elapsed >= period) break;
            if (xTaskNotifyWait(0, 0xFFFFFFFF, &bits, period - elapsed) != pdTRUE) break;
            if (bits & NOTIFY_CONFIG) break;
            if (bits & NOTIFY_READ) read_for_page();
        }
    }
}

void sensor_read_now()
{
    if (s_task != NULL) xTaskNotify(s_task, NOTIFY_READ, eSetBits);
}

// ------------------------------------------------------------------- config

void sensors_init()
{
    String text;
    Config c;
    const char* err = nullptr;
    if (read_file(SENSOR_CONFIG_PATH, text)) {
        if (config_from_text(text.c_str(), c, USED_PINS, USED_PINS_N, &err))
            serial_print(String("Sensor config loaded, mode ") + (c.enabled ? "on" : "off"));
        else
            serial_print(String("Sensor config invalid (") + (err ? err : "?") + "), mode off");
    }
    {
        std::lock_guard<std::mutex> g(s_cfg_lock);
        s_cfg = c;                          // default-constructed (disabled) on any error
    }
    s_enabled.store(c.enabled);
    // Not critical: without the task the mode simply never broadcasts.
    if (xTaskCreatePinnedToCore(sensor_task, "sensors", 6000, NULL, 1, &s_task, 1) != pdPASS) {
        s_enabled.store(false);
        serial_print("sensor task create failed");
    }
}

bool sensor_save_config(const char* json)
{
    Config c;
    const char* err = nullptr;
    if (!config_from_text(json, c, USED_PINS, USED_PINS_N, &err)) {
        show_alert(String("Sensor settings rejected: ") + (err ? err : "invalid"));
        return false;
    }
    if (c.enabled && s_task == NULL) {
        show_alert("Sensor task is not running, restart the device");
        return false;
    }
    if (!write_file_atomic(SENSOR_CONFIG_PATH, String(config_to_text(c).c_str()))) {
        show_alert("Failed to save sensor settings");
        return false;
    }
    {
        std::lock_guard<std::mutex> g(s_cfg_lock);
        s_cfg = c;
    }
    s_enabled.store(c.enabled);
    // Apply now, and read once so the page shows at once whether each sensor answers.
    if (s_task != NULL) xTaskNotify(s_task, NOTIFY_CONFIG | NOTIFY_READ, eSetBits);
    serial_print(String("Sensor config saved, mode ") + (c.enabled ? "on" : "off"));
    return true;
}

void sensor_send_config_to_ws()
{
    Config c = config_snapshot();
    JsonDocument doc;
    doc["response_type"] = "sensor_config";
    config_to_json(c, doc["config"].to<JsonObject>());
    // Usable pins on this board: io = can drive and read, in = can read, adc = analog.
    JsonArray io = doc["pins"]["io"].to<JsonArray>();
    JsonArray in = doc["pins"]["in"].to<JsonArray>();
    JsonArray adc = doc["pins"]["adc"].to<JsonArray>();
    for (int pin = 0; pin <= 39; pin++) {
        if (pin_ok_io(pin, USED_PINS, USED_PINS_N)) io.add(pin);
        if (pin_ok_input(pin, USED_PINS, USED_PINS_N)) in.add(pin);
        if (pin_ok_adc(pin, USED_PINS, USED_PINS_N)) adc.add(pin);
    }
    String out;
    serializeJson(doc, out);
    send_to_ws(out);
}
