#include "lora_support.h"
#include <new>
#include <esp_random.h>
#include <atomic>

using namespace lora_proto;

SemaphoreHandle_t lora_write_mutex = NULL;

static const char* LORA_CONFIG_PATH  = "/config/lora_config.json";
static const char* LORA_DEFAULT_PATH = "/config/lora_default.json";

static TaskHandle_t  rx_task_handle = NULL;
static QueueHandle_t ack_queue = NULL;           // uint16_t: (seq << 8) | value, RX task -> TX task
static volatile bool tx_in_progress = false;     // set while we own the radio for TX; ISR ignores DIO0 then

static portMUX_TYPE seq_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t next_seq = 0;

static uint8_t take_seq()
{
    portENTER_CRITICAL(&seq_mux);
    uint8_t s = next_seq++;
    portEXIT_CRITICAL(&seq_mux);
    return s;
}

// Init failure: report and park this task forever (no busy loop).
static void lora_fatal(const char* msg)
{
    serial_print(String("ERROR: ") + msg);
    oled_set_lora(LORA_ERROR);
    oled_alert("LoRa error", msg, 0);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

// ---------------------------------------------------------------- init / config

void config_lora()
{
    serial_print("Configuring LORA");

    lora_write_mutex = xSemaphoreCreateMutex();
    ack_queue = xQueueCreate(4, sizeof(uint16_t));
    if (lora_write_mutex == NULL || ack_queue == NULL) {
        lora_fatal("LoRa: out of memory");
    }
    next_seq = (uint8_t)esp_random();

    SPI.begin(SCK, MISO, MOSI, SS);
    LoRa.setPins(SS, RST, IRQ);
    if (!LoRa.begin(433E6)) {
        lora_fatal("LoRa init failed, check wiring");
    }
    set_lora_parameters();      // applies config and enters RX mode

    // The RX task must exist before the interrupt can notify it.
    if (xTaskCreatePinnedToCore(lora_rx_task, "lora_rx", 4096, NULL, 2, &rx_task_handle, 1) != pdPASS ||
        xTaskCreatePinnedToCore(LoRa_sendRaw, "LoRa_sendRaw", 6000, NULL, 1, NULL, 1) != pdPASS ||
        xTaskCreatePinnedToCore(manage_recv_queue, "manage_recv_queue", 6000, NULL, 1, NULL, 1) != pdPASS) {
        lora_fatal("LoRa: task create failed");
    }
    // Our own DIO0 handler (not LoRa.onReceive): the library's handler drives SPI
    // from the ISR, which would race with mutex-guarded SPI in tasks.
    pinMode(IRQ, INPUT);
    attachInterrupt(digitalPinToInterrupt(IRQ), onReceive, RISING);
    oled_set_lora(LORA_RECEIVING);
}

// Airtime of a REC_AKNG frame under the active config. Read by the TX task without the mutex.
static std::atomic<uint32_t> s_ack_airtime_ms{0};
// Snapshot of the active radio config for timing estimates (tx_started). Written
// in apply_config_locked, read by the TX task; cfg_mux makes the struct copy atomic.
static portMUX_TYPE cfg_mux = portMUX_INITIALIZER_UNLOCKED;
static LoraConfig s_cfg = default_config();

static LoraConfig active_config()
{
    portENTER_CRITICAL(&cfg_mux);
    LoraConfig c = s_cfg;
    portEXIT_CRITICAL(&cfg_mux);
    return c;
}

// Caller must hold lora_write_mutex.
static void apply_config_locked(const LoraConfig& c)
{
    portENTER_CRITICAL(&cfg_mux);
    s_cfg = c;
    portEXIT_CRITICAL(&cfg_mux);
    s_ack_airtime_ms = airtime_ms(c.SpreadingFactor, c.SignalBandwidth, c.CodingRate4, ACK_FRAME_LEN);
    LoRa.idle();
    LoRa.setFrequency(c.freq);
    LoRa.setTxPower(c.TxPower);
    LoRa.setSpreadingFactor(c.SpreadingFactor);
    LoRa.setSignalBandwidth(c.SignalBandwidth);
    LoRa.setCodingRate4(c.CodingRate4);
    LoRa.setSyncWord(c.SyncWord);
    // Hardware payload CRC on TX. In explicit-header mode the receiver reads the CRC
    // flag from each packet's header, so peers with or without this still interoperate.
    // A corrupted packet is then dropped by the radio (parsePacket returns 0) before our
    // crc8 runs; the sender times out and retries.
    LoRa.enableCrc();
    LoRa_rxMode();
}

static bool apply_config(const LoraConfig& c)
{
    if (xSemaphoreTake(lora_write_mutex, pdMS_TO_TICKS(LORA_TX_TIMEOUT_MS)) != pdTRUE) {
        serial_print("ERROR: LoRa config mutex timeout");
        return false;
    }
    apply_config_locked(c);
    xSemaphoreGive(lora_write_mutex);
    oled_set_lora_params(c.freq, c.SpreadingFactor, c.SignalBandwidth);
    return true;
}

static bool read_config_file(const char* path, LoraConfig& out)
{
    String text;
    // read_file honours the FS lock (refuses during a filesystem OTA remount).
    if (!read_file(path, text) || text.length() > 1024) return false;
    const char* err = nullptr;
    if (!config_from_text(text.c_str(), out, &err)) {
        serial_print(String("LoRa config ") + path + " invalid (" + err + ")");
        return false;
    }
    return true;
}

void set_lora_parameters()
{
    LoraConfig c;
    if (read_config_file(LORA_CONFIG_PATH, c)) {
        serial_print("LoRa config: lora_config.json");
    } else if (read_config_file(LORA_DEFAULT_PATH, c)) {
        serial_print("LoRa config: lora_default.json (fallback)");
    } else {
        c = default_config();
        serial_print("LoRa config: compiled defaults (fallback)");
    }
    serial_print("LoRa freq=" + String(c.freq) + " SF=" + String(c.SpreadingFactor) +
                 " BW=" + String(c.SignalBandwidth) + " CR=" + String(c.CodingRate4) +
                 " TxP=" + String(c.TxPower) + " SW=" + String(c.SyncWord));
    if (!apply_config(c)) lora_fatal("LoRa: cannot apply config");
}

bool save_lora_config(String value)
{
    LoraConfig c;
    const char* err = nullptr;
    if (!config_from_text(value.c_str(), c, &err)) {
        show_alert(String("Invalid LoRa config: ") + (err ? err : "?") + " missing or out of range");
        return false;
    }
    char js[192];
    if (config_to_json(c, js, sizeof js) == 0 || !write_file_atomic(LORA_CONFIG_PATH, String(js))) {
        show_alert("Failed to save LoRa config");
        return false;
    }
    if (!apply_config(c)) {
        show_alert("LoRa config saved, radio busy: it applies after restart");
        return false;
    }
    serial_print("LoRa config saved");
    show_alert("LoRa config saved successfully");
    return true;
}

// ---------------------------------------------------------------- radio modes
// IQ: NORMAL (non-inverted) for both TX and RX, on every packet (data and ACK).
// Before this, RX used inverted IQ and TX normal IQ (the library's node/gateway
// example). Two boards running the same firmware then could never decode each other:
// confirmed in two-board HIL, where neither board logged any RX. v1 got away with it
// because it called endPacket(true) and then LoRa_rxMode() straight away, which
// rewrote the IQ registers while the packet was going out. Peers must use the same
// setting: boards in the field on older v2 builds need this update to talk to new ones.

// Switch radio to continuous receive mode.
void LoRa_rxMode(){
    LoRa.disableInvertIQ();               // normal IQ (same as TX)
    LoRa.receive();                       // continuous RX, explicit header, DIO0 => RxDone
}

// Switch radio to idle/transmit mode.
void LoRa_txMode(){
    LoRa.idle();                          // set standby mode
    LoRa.disableInvertIQ();               // normal IQ (same as RX)
}

// ---------------------------------------------------------------- TX

// Blocking transmit of one frame, then back to RX. Takes lora_write_mutex.
// Note: the library's endPacket() spins on yield() until TX_DONE with no timeout, and
// LoRaClass::isTransmitting() is private in LoRa@0.8.0, so a TX-hang guard would need
// a library change (reported, not done).
static bool lora_tx_frame(const uint8_t* frame, size_t n)
{
    if (xSemaphoreTake(lora_write_mutex, pdMS_TO_TICKS(LORA_TX_TIMEOUT_MS)) != pdTRUE) {
        serial_print("ERROR: LoRa write mutex timeout");
        return false;
    }
    tx_in_progress = true;
    LoRa_txMode();
    bool ok = LoRa.beginPacket() == 1;
    if (ok) {
        LoRa.write(frame, n);
        LoRa.endPacket();                 // blocking: returns after TX_DONE
    }
    LoRa_rxMode();
    tx_in_progress = false;
    xSemaphoreGive(lora_write_mutex);
    return ok;
}

bool LoRa_send(const String& data, uint8_t type, uint8_t seq)
{
    if (data.length() > LORA_MAX_PAYLOAD) {
        serial_print("ERROR: LoRa payload " + String(data.length()) + " > " + String(LORA_MAX_PAYLOAD));
        return false;
    }
    uint8_t frame[MAX_FRAME];
    size_t n = frame_packet(type, seq, (const uint8_t*)data.c_str(), data.length(), frame, sizeof frame);
    if (n == 0) {
        serial_print("ERROR: LoRa frame rejected, type " + String(type));
        return false;
    }
    bool ok = lora_tx_frame(frame, n);
    if (ok) serial_print("LoRa TX type=" + String(type) + " seq=" + String(seq) + " len=" + String(data.length()));
    led_nortifier();                      // outside the mutex
    return ok;
}

bool LoRa_send(const String& data, uint8_t type)
{
    return LoRa_send(data, type, take_seq());
}

void LoRa_sendAkn(uint8_t seq, uint8_t result)
{
    uint8_t frame[HEADER_LEN + 1];
    size_t n = frame_ack(seq, result, frame, sizeof frame);
    if (lora_tx_frame(frame, n))
        serial_print("SENT AKN seq=" + String(seq) + " val=" + String(result));
}

static void send_tx_result(uint32_t id, bool ok)
{
    JsonDocument doc;
    doc["response_type"] = "tx_result";
    doc["id"] = id;
    doc["akn"] = ok ? 1 : 0;
    doc["username"] = get_username();
    String out;
    serializeJson(doc, out);
    send_to_ws(out);
}

// Contract 3: tx_started {id, expected_ms, attempts}, sent when the TX task takes the
// item off send_packets. expected_ms is the worst case until tx_result (every attempt
// times out) under the active config.
static void send_tx_started(uint32_t id, size_t payload_len)
{
    const LoraConfig c = active_config();
    JsonDocument doc;
    doc["response_type"] = "tx_started";
    doc["id"] = id;
    doc["expected_ms"] = tx_expected_ms(c.SpreadingFactor, c.SignalBandwidth, c.CodingRate4,
                                        HEADER_LEN + payload_len, LORA_ACK_TIMEOUT_MS);
    doc["attempts"] = TX_ATTEMPTS;
    String out;
    serializeJson(doc, out);
    send_to_ws(out);
}

// Sends one queued packet with ACK/retry. Returns true on ACK 1.
static bool send_with_retry(const QueueParam* p)
{
    if (p->type != LORA_MSG) {
        serial_print("ERROR: send_packets item has type " + String(p->type));
        return false;
    }
    if (p->message.length() > LORA_MAX_PAYLOAD) {
        serial_print("ERROR: send_packets item too long: " + String(p->message.length()));
        return false;
    }
    // LORA_ACK_TIMEOUT_MS covers the peer's processing time. The ACK's own airtime is
    // added on top: about 10 ms at the defaults, but about 15 s at SF12/BW7.8k, where a
    // flat 5 s would make every ACK arrive too late.
    const TickType_t ack_timeout = pdMS_TO_TICKS(LORA_ACK_TIMEOUT_MS + s_ack_airtime_ms.load());
    uint32_t backoff_max;
    {
        const LoraConfig c = active_config();
        backoff_max = retry_backoff_max_ms(
            airtime_ms(c.SpreadingFactor, c.SignalBandwidth, c.CodingRate4, HEADER_LEN + p->message.length()),
            s_ack_airtime_ms.load());
    }
    TxRetry t;
    tx_begin(t, take_seq());
    xQueueReset(ack_queue);               // stale ACKs from earlier packets

    for (;;) {
        TxAction a = TxAction::Wait;
        const uint8_t attempt = t.attempts;   // 1-based number of the transmission below
        const String tag = "/" + String(MAX_RETRIES + 1);   // total transmissions
        if (!LoRa_send(p->message, p->type, t.seq)) {
            a = tx_on_timeout(t);         // radio busy counts as a failed attempt
            serial_print("TX failed (radio busy) seq=" + String(t.seq) + ", attempt " + String(attempt) + tag);
        } else {
            TickType_t start = xTaskGetTickCount();
            while (a == TxAction::Wait) {
                TickType_t elapsed = xTaskGetTickCount() - start;
                uint16_t v;
                if (elapsed >= ack_timeout || xQueueReceive(ack_queue, &v, ack_timeout - elapsed) != pdTRUE) {
                    a = tx_on_timeout(t);
                    serial_print("ACK timeout seq=" + String(t.seq) + ", attempt " + String(attempt) + tag);
                    break;
                }
                uint8_t ack_seq = v >> 8, val = v & 0xFF;
                a = tx_on_ack(t, ack_seq, val);
                if (a == TxAction::Wait)
                    serial_print("Ignored ACK seq=" + String(ack_seq) + " (want " + String(t.seq) + ")");
                else if (a != TxAction::Success)
                    serial_print("NACK " + String(val) + " seq=" + String(t.seq) + ", attempt " + String(attempt) + tag);
            }
        }
        if (a == TxAction::Success) return true;
        if (a == TxAction::Fail) return false;
        // Retransmit after a random backoff (breaks lockstep with a peer that's also sending).
        vTaskDelay(pdMS_TO_TICKS(retry_backoff_ms(esp_random(), backoff_max)));
    }
}

void LoRa_sendRaw(void* param)
{
    serial_print("LoRa_sendRaw");
    for (;;) {
        QueueParam* p = NULL;
        if (xQueueReceive(send_packets, &p, portMAX_DELAY) != pdTRUE || p == NULL) continue;

        if (p->id != 0) send_tx_started(p->id, p->message.length());
        oled_set_lora(LORA_TRANSMITTING);
        bool ok = send_with_retry(p);
        oled_set_lora(LORA_RECEIVING);
        oled_lora_packet(true, ok, 0, 0.0f);

        if (!ok) {
            show_alert("Transmission failed after " + String(LORA_MAX_RETRIES) + " retries");
            oled_alert("TX failed", "no ACK from peer", 3000);
        }
        if (p->id != 0) send_tx_result(p->id, ok);
        delete p;
    }
}

// ---------------------------------------------------------------- RX

void IRAM_ATTR onReceive()
{
    // ISR: no SPI, no allocation, no logging. Our own TX_DONE edge is ignored.
    if (tx_in_progress || rx_task_handle == NULL) return;
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(rx_task_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

static RxDedupMulti rx_dedup;             // only touched by lora_rx_task, per-source tracking

static void handle_rx_frame(const uint8_t* buf, size_t n, int rssi, float snr)
{
    Parsed p = parse_packet(buf, n);
    if (!p.ok) {
        serial_print("LoRa RX invalid frame (err " + String((int)p.err) + ", " + String(n) + " bytes)");
        oled_lora_packet(false, false, rssi, snr);
        // A data packet with a bad CRC gets a NACK so the sender retries early.
        // If the seq byte itself is corrupt, the sender ignores the mismatched NACK.
        if (p.err == ParseErr::BadCrc && p.type == LORA_MSG)
            LoRa_sendAkn(p.seq, ACK_RETRY);
        return;
    }

    char text[MAX_PAYLOAD + 1];
    memcpy(text, p.payload, p.len);
    text[p.len] = '\0';

    switch (p.type) {
    case REC_AKNG: {
        uint16_t v = ((uint16_t)p.seq << 8) | p.payload[0];
        xQueueSend(ack_queue, &v, 0);
        serial_print("REC AKN seq=" + String(p.seq) + " val=" + String(p.payload[0]));
        return;
    }
    case LORA_SERIAL: {
        oled_lora_packet(false, true, rssi, snr);
        DebugQueueParam* d = new (std::nothrow) DebugQueueParam();
        if (d == NULL) return;
        // Binary-safe: bridged serial bytes may include 0x00, so copy by length, never via strlen.
        d->message.concat((const char*)p.payload, p.len);
        if (d->message.length() != p.len) { delete d; return; }
        if (xQueueSend(serial_packet_rec, &d, 0) != pdTRUE) delete d;
        return;
    }
    case RAW_DATA:
        // Retired type (old chunked file transfer from older firmware): drop, no ACK.
        serial_print("LoRa RX RAW_DATA seq=" + String(p.seq) + " ignored (file transfer retired)");
        return;
    default: {   // LORA_MSG
        // Per-source dedup keyed by the sender's MAC hash.
        uint32_t dedup_key = extract_mac_key(text);
        if (rx_is_duplicate_multi(rx_dedup, dedup_key, p.seq)) {
            LoRa_sendAkn(p.seq, ACK_OK);
            serial_print("LoRa RX duplicate seq=" + String(p.seq) + ", re-ACKed");
            return;
        }
        bool queued = false;
        QueueParam* q = new (std::nothrow) QueueParam();
        if (q != NULL) {
            q->type = p.type;
            q->message = text;
            q->id = 0;
            queued = q->message.length() == p.len && xQueueSend(recv_packets, &q, 0) == pdTRUE;
            if (!queued) delete q;
        }
        if (queued) rx_commit_multi(rx_dedup, dedup_key, p.seq);
        LoRa_sendAkn(p.seq, queued ? ACK_OK : ACK_RETRY);
        oled_lora_packet(false, queued, rssi, snr);
        serial_print("LoRa RX type=" + String(p.type) + " seq=" + String(p.seq) + " len=" + String(p.len) +
                     " rssi=" + String(rssi) + " snr=" + String(snr, 1) + (queued ? "" : " DROPPED (queue full)"));
        return;
    }
    }
}

void lora_rx_task(void* param)
{
    uint8_t buf[256];
    for (;;) {
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        // Missed-edge recovery: DIO0 still high means an unread RxDone.
        if (notified == 0 && digitalRead(IRQ) != HIGH) continue;

        if (xSemaphoreTake(lora_write_mutex, pdMS_TO_TICKS(LORA_TX_TIMEOUT_MS)) != pdTRUE) {
            continue;   // DIO0 stays high; the recovery check above retries
        }
        size_t n = 0;
        int rssi = 0;
        float snr = 0;
        int size = LoRa.parsePacket();    // clears IRQ flags; 0 on CRC error / spurious wake
        if (size > 0) {
            while (n < sizeof buf && LoRa.available()) buf[n++] = (uint8_t)LoRa.read();
            rssi = LoRa.packetRssi();
            snr = LoRa.packetSnr();
        }
        LoRa_rxMode();                    // parsePacket leaves standby / single-RX mode
        xSemaphoreGive(lora_write_mutex);

        if (size > 0) {
            if ((size_t)size != n) serial_print("LoRa RX short read " + String(n) + "/" + String(size));
            handle_rx_frame(buf, n, rssi, snr);
        }
    }
}

// ---------------------------------------------------------------- forwarding

static void handle_lora_msg_for_oled(const String& payload)
{
    LoraMsgInfo m = parse_lora_msg(payload.c_str());
    switch (m.kind) {
    case MsgKind::Text:
        oled_message(m.from.c_str(), m.text.c_str());
        break;
    case MsgKind::Sos:
        oled_alert("SOS", m.from.c_str(), 5000);
        break;
    default:
        break;
    }
}

void manage_recv_queue(void* param)
{
    for (;;) {
        QueueParam* p = NULL;
        if (xQueueReceive(recv_packets, &p, portMAX_DELAY) != pdTRUE || p == NULL) continue;
        led_nortifier();
        if (p->type == LORA_MSG) {
            send_msg_to_ws(p->message);
            handle_lora_msg_for_oled(p->message);
        }
        delete p;
    }
}

void send_msg_to_ws(const String& data)
{
    JsonDocument doc;
    doc["response_type"] = "lora_rx";
    doc["lora_msg"] = data;
    String val;
    serializeJson(doc, val);
    send_to_ws(val);
}
