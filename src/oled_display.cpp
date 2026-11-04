// OLED UI (owner: oled-display-engineer). Implements Contract 7.
//
// Design: public calls build a fixed-size OledMsg (strings copied into char arrays)
// and xQueueSend it with a zero timeout. A single display task owns all UI state and
// the only I2C traffic to the panel. If no panel was found (or memory ran out) the
// queue handle stays null and every call returns immediately.
#include "oled_display.h"
#include "oled_layout.h"

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_timer.h>

using namespace oled_layout;

#ifndef PROJECT_NAME
#define PROJECT_NAME "Voyager"
#endif
#ifndef OSDA
#define OSDA 21
#endif
#ifndef OSCL
#define OSCL 22
#endif

namespace {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
const uint8_t OLED_ADDR = 0x3C;
const UBaseType_t QUEUE_DEPTH = 12;
const uint32_t TASK_STACK = 4096;
const UBaseType_t TASK_PRIO = 1;       // below radio / network tasks
const BaseType_t TASK_CORE = 1;
const uint32_t I2C_HZ = 400000;
const uint32_t MSG_OVERLAY_MS = 2500;  // "New message from X"
const uint32_t PROGRESS_STALE_MS = 60000;  // progress overlay hides if not updated
const uint32_t MIN_FRAME_MS = 100;     // caps I2C frames at 10 Hz
const uint32_t IDLE_TICK_MS = 1000;
const uint32_t BLINK_MS = 500;

// Screen layout (size-1 font: 6x8 px, 21 chars per 128 px line)
const int LINE_CHARS = SCREEN_W / CHAR_W;  // 21
const int HEADER_LINE_Y = 9;
const int CONTENT_Y = 11;
const int LINE_STEP = 9;
const int CONTENT_LINES = 5;  // y = 11, 20, 29, 38, 47
const int DOTS_Y = 60;

// ---------------------------------------------------------------------------
// Queue message
// ---------------------------------------------------------------------------
enum Cmd : uint8_t {
    CMD_WIFI,
    CMD_AP,
    CMD_WIFI_RSSI,
    CMD_LORA,
    CMD_LORA_PKT,
    CMD_LORA_PARAMS,
    CMD_MESSAGE,
    CMD_ALERT,
    CMD_PROGRESS,
    CMD_BUTTON,
    CMD_STATUS,
};

struct OledMsg {
    uint8_t cmd;
    uint8_t u8;       // status enum / button / pct
    bool b0, b1;      // ssid given, ip given / tx, ok / long press
    int32_t i0, i1, i2;
    float f;
    uint32_t ms;
    char s0[MSG_FROM_LEN];  // ssid / title / from
    char s1[MSG_TEXT_LEN];  // text / status
    char s2[65];            // AP password (max 63)
    char s3[16];            // dotted IPv4
};

// ---------------------------------------------------------------------------
// UI state: touched only by the display task
// ---------------------------------------------------------------------------
struct UiState {
    WiFiStatus wifi;
    char ssid[MSG_FROM_LEN];
    char ip[16];
    bool ap_known;
    char ap_ssid[MSG_FROM_LEN];
    char ap_pass[65];
    char ap_ip[16];
    int wifi_rssi;

    LoRaStatus lora;
    bool params_known;
    long freq, bw;
    int sf;
    bool rx_seen;
    int last_rssi;
    float last_snr;
    uint32_t tx_ok, tx_fail, rx;

    char status[MSG_TEXT_LEN];

    MsgRing ring;
    int8_t msg_sel;  // -1 list, else detail index
    uint8_t page;

    bool alert_on, alert_sticky;
    uint32_t alert_until;
    char alert_title[MSG_FROM_LEN];
    char alert_text[MSG_TEXT_LEN];

    bool prog_on;
    uint8_t prog_pct;
    uint32_t prog_last;
    char prog_title[MSG_FROM_LEN];

    bool msgov_on;
    uint32_t msgov_until;
    char msgov_from[MSG_FROM_LEN];
    char msgov_text[MSG_TEXT_LEN];

    SleepState sleep;
    bool panel_on;
    bool dirty;
    uint32_t last_draw;
};

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1, I2C_HZ, I2C_HZ);
UiState ui;
OledMsg rx_msg;  // display task's receive buffer (kept off its stack)
QueueHandle_t volatile s_queue = nullptr;
uint32_t s_dropped = 0;

// ---------------------------------------------------------------------------
// Producer side (any task)
// ---------------------------------------------------------------------------
inline bool api_ready() { return s_queue != nullptr && !xPortInIsrContext(); }

void post(const OledMsg& m) {
    QueueHandle_t q = s_queue;
    if (!q) return;
    if (xQueueSend(q, &m, 0) != pdTRUE) __atomic_fetch_add(&s_dropped, 1, __ATOMIC_RELAXED);
}

inline void msg_init(OledMsg& m, Cmd c) {
    memset(&m, 0, sizeof(m));
    m.cmd = c;
}

// ---------------------------------------------------------------------------
// Drawing helpers (display task only)
// ---------------------------------------------------------------------------
void print_at(int x, int y, const char* text, int max_chars = LINE_CHARS) {
    char buf[LINE_CHARS + 1];
    int fit = chars_fit(SCREEN_W - (x < 0 ? 0 : x));
    if (max_chars > fit) max_chars = fit;
    truncate_ellipsis(text, buf, sizeof(buf), max_chars);
    display.setCursor(clamp_x(x, text_width(buf)), y);
    display.print(buf);
}

void print_line(int line, const char* text) { print_at(0, CONTENT_Y + line * LINE_STEP, text); }

void print_centered(int y, const char* text, uint8_t size = 1) {
    char buf[LINE_CHARS + 1];
    truncate_ellipsis(text, buf, sizeof(buf), chars_fit(SCREEN_W, size));
    display.setTextSize(size);
    display.setCursor(center_x(text_width(buf, size)), y);
    display.print(buf);
    display.setTextSize(1);
}

// Word-wrapped block at (x, y); returns lines drawn.
int draw_wrapped(int x, int y, int max_px, int max_lines, const char* text, int step = LINE_STEP) {
    char lines[CONTENT_LINES + 1][LINE_CHARS + 1];
    if (max_lines > CONTENT_LINES + 1) max_lines = CONTENT_LINES + 1;
    int n = wrap_text(text, chars_fit(max_px), max_lines, &lines[0][0], sizeof(lines[0]));
    for (int i = 0; i < n; i++) print_at(x, y + i * step, lines[i]);
    return n;
}

// Hard-split "label value" over up to max_lines content lines starting at line.
int draw_split(int line, int max_lines, const char* label, const char* value) {
    char joined[MSG_TEXT_LEN];
    snprintf(joined, sizeof(joined), "%s%s", label, value ? value : "");
    char lines[3][LINE_CHARS + 1];
    if (max_lines > 3) max_lines = 3;
    int n = split_lines(joined, LINE_CHARS, max_lines, &lines[0][0], sizeof(lines[0]));
    for (int i = 0; i < n; i++) print_line(line + i, lines[i]);
    return n;
}

void draw_wifi_icon(int x, int y, WiFiStatus st, uint32_t now) {
    if (st == WIFI_DISCONNECTED) {
        display.drawLine(x, y, x + 6, y + 6, SSD1306_WHITE);
        display.drawLine(x, y + 6, x + 6, y, SSD1306_WHITE);
        return;
    }
    if (st == WIFI_CONNECTING && ((now / BLINK_MS) % 2) == 1) return;  // blink
    display.drawLine(x + 1, y, x + 5, y, SSD1306_WHITE);
    display.drawPixel(x, y + 1, SSD1306_WHITE);
    display.drawPixel(x + 6, y + 1, SSD1306_WHITE);
    display.drawLine(x + 2, y + 3, x + 4, y + 3, SSD1306_WHITE);
    display.drawPixel(x + 1, y + 4, SSD1306_WHITE);
    display.drawPixel(x + 5, y + 4, SSD1306_WHITE);
    display.drawLine(x + 3, y + 5, x + 3, y + 6, SSD1306_WHITE);
    if (st == WIFI_CONNECTED_AP) {
        display.setCursor(x + 8, y);
        display.print('A');
    }
}

void draw_lora_icon(int x, int y, LoRaStatus st) {
    if (st == LORA_ERROR) {
        display.drawLine(x, y, x + 6, y + 6, SSD1306_WHITE);
        display.drawLine(x, y + 6, x + 6, y, SSD1306_WHITE);
        return;
    }
    display.drawLine(x + 3, y + 1, x + 3, y + 6, SSD1306_WHITE);  // mast
    if (st == LORA_TRANSMITTING) {                                 // arrow out
        display.drawLine(x + 3, y, x, y + 3, SSD1306_WHITE);
        display.drawLine(x + 3, y, x + 6, y + 3, SSD1306_WHITE);
    } else if (st == LORA_RECEIVING) {                             // arrow in
        display.drawLine(x, y, x + 3, y + 3, SSD1306_WHITE);
        display.drawLine(x + 6, y, x + 3, y + 3, SSD1306_WHITE);
    } else {                                                       // idle antenna
        display.drawLine(x, y + 1, x + 3, y + 3, SSD1306_WHITE);
        display.drawLine(x + 6, y + 1, x + 3, y + 3, SSD1306_WHITE);
    }
}

void draw_signal_bars(int x, int y, uint8_t bars) {
    for (int i = 0; i < 4; i++) {
        int h = (i + 1) * 2 - 1;  // 1,3,5,7
        int bx = x + i * 3;
        int by = y + 7 - h;
        if (i < bars) display.fillRect(bx, by, 2, h, SSD1306_WHITE);
        else display.drawPixel(bx, y + 6, SSD1306_WHITE);
    }
}

void draw_progress_bar(int x, int y, int w, int h, uint8_t pct) {
    display.drawRect(x, y, w, h, SSD1306_WHITE);
    int fill = progress_fill(w - 2, pct);
    if (fill > 0) display.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
}

// Header: page title on the left; WiFi, LoRa and signal icons on the right.
void draw_header(const char* title, uint32_t now) {
    print_at(0, 0, title, 13);
    draw_wifi_icon(86, 1, ui.wifi, now);
    draw_lora_icon(103, 1, ui.lora);
    uint8_t bars = ui.wifi == WIFI_CONNECTED_STA ? rssi_to_bars(ui.wifi_rssi) : 0;
    draw_signal_bars(116, 1, bars);
    display.drawFastHLine(0, HEADER_LINE_Y, SCREEN_W, SSD1306_WHITE);
}

void draw_page_dots() {
    const int spacing = 6;
    int total = (PAGE_COUNT - 1) * spacing + 3;
    int x0 = center_x(total) + 1;
    for (int i = 0; i < PAGE_COUNT; i++) {
        int cx = x0 + i * spacing;
        if (i == ui.page) display.fillRect(cx - 1, DOTS_Y, 3, 3, SSD1306_WHITE);
        else display.drawPixel(cx, DOTS_Y + 1, SSD1306_WHITE);
    }
}

const char* lora_state_text(LoRaStatus s) {
    switch (s) {
        case LORA_TRANSMITTING: return "TX";
        case LORA_RECEIVING: return "RX";
        case LORA_ERROR: return "ERROR";
        default: return "idle";
    }
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------
void page_status(uint32_t now) {
    draw_header(PROJECT_NAME, now);
    char buf[64];
    switch (ui.wifi) {
        case WIFI_CONNECTED_AP: snprintf(buf, sizeof(buf), "AP  %s", ui.ssid); break;
        case WIFI_CONNECTED_STA: snprintf(buf, sizeof(buf), "STA %s", ui.ssid); break;
        case WIFI_CONNECTING: snprintf(buf, sizeof(buf), "Joining %s", ui.ssid); break;
        default: snprintf(buf, sizeof(buf), "WiFi offline"); break;
    }
    print_line(0, buf);
    snprintf(buf, sizeof(buf), "IP  %s", ui.ip[0] ? ui.ip : "--");
    print_line(1, buf);
    snprintf(buf, sizeof(buf), "LoRa %s  rx %lu", lora_state_text(ui.lora), (unsigned long)ui.rx);
    print_line(2, buf);
    draw_wrapped(0, CONTENT_Y + 3 * LINE_STEP, SCREEN_W, 2, ui.status);
}

void page_messages(uint32_t now) {
    char title[16];
    if (ui.msg_sel >= 0) snprintf(title, sizeof(title), "Msg %d/%d", ui.msg_sel + 1, ui.ring.count);
    else snprintf(title, sizeof(title), "Messages");
    draw_header(title, now);
    if (ui.ring.count == 0) {
        print_centered(CONTENT_Y + 2 * LINE_STEP, "No messages");
        return;
    }
    if (ui.msg_sel >= 0 && ui.msg_sel < ui.ring.count) {
        // Detail view: inverted sender bar + wrapped text.
        int y = CONTENT_Y;
        display.fillRect(0, y - 1, SCREEN_W, LINE_STEP, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
        print_at(1, y, ring_from(ui.ring, ui.msg_sel));
        display.setTextColor(SSD1306_WHITE);
        draw_wrapped(0, y + LINE_STEP, SCREEN_W, CONTENT_LINES - 1, ring_text(ui.ring, ui.msg_sel));
        return;
    }
    // List view: newest first, one line each.
    char buf[MSG_FROM_LEN + MSG_TEXT_LEN + 4];
    for (int i = 0; i < ui.ring.count && i < CONTENT_LINES; i++) {
        snprintf(buf, sizeof(buf), "%s: %s", ring_from(ui.ring, i), ring_text(ui.ring, i));
        for (char* c = buf; *c; c++) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
        print_line(i, buf);
    }
}

void page_lora(uint32_t now) {
    draw_header("LoRa", now);
    char buf[40];
    if (ui.params_known) {
        snprintf(buf, sizeof(buf), "Freq %ld.%03ld MHz", ui.freq / 1000000L, (ui.freq / 1000L) % 1000L);
        print_line(0, buf);
        char bw[12];
        if (ui.bw % 1000L == 0) snprintf(bw, sizeof(bw), "%ldk", ui.bw / 1000L);
        else snprintf(bw, sizeof(bw), "%ld.%ldk", ui.bw / 1000L, (ui.bw % 1000L) / 100L);
        snprintf(buf, sizeof(buf), "SF %d   BW %s", ui.sf, bw);
        print_line(1, buf);
    } else {
        print_line(0, "Freq --");
        print_line(1, "SF --   BW --");
    }
    if (ui.rx_seen) snprintf(buf, sizeof(buf), "RSSI %d  SNR %.1f", ui.last_rssi, (double)ui.last_snr);
    else snprintf(buf, sizeof(buf), "RSSI --  SNR --");
    print_line(2, buf);
    snprintf(buf, sizeof(buf), "TX ok %lu  fail %lu", (unsigned long)ui.tx_ok, (unsigned long)ui.tx_fail);
    print_line(3, buf);
    snprintf(buf, sizeof(buf), "RX %lu  (%s)", (unsigned long)ui.rx, lora_state_text(ui.lora));
    print_line(4, buf);
}

void page_network(uint32_t now) {
    draw_header("Network", now);
    char buf[40];
    switch (ui.wifi) {
        case WIFI_CONNECTED_AP: {
            const char* ssid = ui.ap_known && ui.ap_ssid[0] ? ui.ap_ssid : ui.ssid;
            const char* ip = ui.ap_known && ui.ap_ip[0] ? ui.ap_ip : ui.ip;
            snprintf(buf, sizeof(buf), "AP   %s", ip[0] ? ip : "--");
            print_line(0, buf);
            draw_split(1, 2, "SSID ", ssid);
            draw_split(3, 2, "Pass ", ui.ap_known ? ui.ap_pass : "--");
            break;
        }
        case WIFI_CONNECTED_STA:
            snprintf(buf, sizeof(buf), "STA  %s", ui.ip[0] ? ui.ip : "--");
            print_line(0, buf);
            draw_split(1, 2, "SSID ", ui.ssid);
            if (ui.wifi_rssi < 0) snprintf(buf, sizeof(buf), "RSSI %d dBm", ui.wifi_rssi);
            else snprintf(buf, sizeof(buf), "RSSI --");
            print_line(3, buf);
            break;
        case WIFI_CONNECTING:
            print_line(0, "STA  joining...");
            draw_split(1, 2, "SSID ", ui.ssid);
            break;
        default:
            print_line(0, "WiFi offline");
            if (ui.ssid[0]) draw_split(1, 2, "Last ", ui.ssid);
            break;
    }
}

void page_system(uint32_t now) {
    draw_header("System", now);
    char buf[40];
    snprintf(buf, sizeof(buf), "Heap %lu B", (unsigned long)ESP.getFreeHeap());
    print_line(0, buf);
    snprintf(buf, sizeof(buf), "Min  %lu B", (unsigned long)ESP.getMinFreeHeap());
    print_line(1, buf);
    char up[24];
    format_uptime((uint64_t)(esp_timer_get_time() / 1000000LL), up, sizeof(up));
    snprintf(buf, sizeof(buf), "Up   %s", up);
    print_line(2, buf);
    snprintf(buf, sizeof(buf), "Built %s", __DATE__);
    print_line(3, buf);
    snprintf(buf, sizeof(buf), "Drops %lu", (unsigned long)__atomic_load_n(&s_dropped, __ATOMIC_RELAXED));
    print_line(4, buf);
}

// ---------------------------------------------------------------------------
// Overlays
// ---------------------------------------------------------------------------
void overlay_alert() {
    display.drawRect(0, 0, SCREEN_W, SCREEN_H, SSD1306_WHITE);
    display.fillRect(0, 0, SCREEN_W, 11, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    print_centered(2, ui.alert_title[0] ? ui.alert_title : "Alert");
    display.setTextColor(SSD1306_WHITE);
    draw_wrapped(4, 14, SCREEN_W - 8, 5, ui.alert_text);
}

void overlay_progress() {
    display.drawRect(0, 0, SCREEN_W, SCREEN_H, SSD1306_WHITE);
    draw_wrapped(4, 8, SCREEN_W - 8, 2, ui.prog_title);
    draw_progress_bar(8, 30, SCREEN_W - 16, 10, ui.prog_pct);
    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)ui.prog_pct);
    print_centered(46, buf);
}

void overlay_message() {
    display.drawRect(0, 0, SCREEN_W, SCREEN_H, SSD1306_WHITE);
    print_centered(5, "New message from");
    const char* from = ui.msgov_from[0] ? ui.msgov_from : "?";
    if (text_width(from, 2) <= SCREEN_W - 4) print_centered(20, from, 2);
    else print_centered(24, from);
    char buf[MSG_TEXT_LEN];
    safe_copy(buf, sizeof(buf), ui.msgov_text);
    for (char* c = buf; *c; c++) if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
    char line[LINE_CHARS + 1];
    truncate_ellipsis(buf, line, sizeof(line), LINE_CHARS - 1);
    print_centered(44, line);
}

void render(uint32_t now) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    if (ui.alert_on) overlay_alert();
    else if (ui.prog_on) overlay_progress();
    else if (ui.msgov_on) overlay_message();
    else {
        switch (ui.page) {
            case PAGE_MESSAGES: page_messages(now); break;
            case PAGE_LORA: page_lora(now); break;
            case PAGE_NETWORK: page_network(now); break;
            case PAGE_SYSTEM: page_system(now); break;
            default: page_status(now); break;
        }
        draw_page_dots();
    }
    display.display();
    ui.last_draw = now;
    ui.dirty = false;
}

// ---------------------------------------------------------------------------
// Display task
// ---------------------------------------------------------------------------
void land_on_messages() {
    ui.msgov_on = false;
    ui.page = PAGE_MESSAGES;
    ui.msg_sel = ui.ring.count > 0 ? 0 : -1;
}

void handle_button(const OledMsg& m, uint32_t now) {
    switch (sleep_button(ui.sleep, m.u8, m.b1, now)) {
        case BTN_ACT_NEXT_PAGE:
            if (ui.alert_on) { ui.alert_on = false; break; }   // press dismisses the alert
            if (ui.msgov_on) { land_on_messages(); break; }
            ui.page = next_page(ui.page);
            ui.msg_sel = -1;
            break;
        case BTN_ACT_BTN1:
            if (ui.alert_on) { ui.alert_on = false; break; }
            if (ui.msgov_on) { land_on_messages(); break; }
            if (ui.page == PAGE_MESSAGES) ui.msg_sel = msg_select_next(ui.msg_sel, ui.ring.count);
            break;
        default:  // WAKE (consumed), SCREEN_OFF, NONE
            break;
    }
}

void apply(const OledMsg& m, uint32_t now) {
    switch (m.cmd) {
        case CMD_WIFI:
            ui.wifi = (WiFiStatus)m.u8;
            if (m.b0) safe_copy(ui.ssid, sizeof(ui.ssid), m.s0);
            if (m.b1) safe_copy(ui.ip, sizeof(ui.ip), m.s3);
            break;
        case CMD_AP:
            ui.ap_known = true;
            safe_copy(ui.ap_ssid, sizeof(ui.ap_ssid), m.s0);
            safe_copy(ui.ap_pass, sizeof(ui.ap_pass), m.s2);
            safe_copy(ui.ap_ip, sizeof(ui.ap_ip), m.s3);
            break;
        case CMD_WIFI_RSSI:
            if (ui.wifi_rssi == m.i0) return;  // 1 Hz feed: redraw only on change
            ui.wifi_rssi = m.i0;
            break;
        case CMD_LORA:
            ui.lora = (LoRaStatus)m.u8;
            break;
        case CMD_LORA_PKT:
            if (m.b0) {
                if (m.b1) ui.tx_ok++;
                else ui.tx_fail++;
            } else {
                ui.rx++;
                ui.rx_seen = true;
                ui.last_rssi = m.i0;
                ui.last_snr = m.f;
            }
            break;
        case CMD_LORA_PARAMS:
            ui.params_known = true;
            ui.freq = m.i0;
            ui.sf = m.i1;
            ui.bw = m.i2;
            break;
        case CMD_MESSAGE:
            ring_push(ui.ring, m.s0, m.s1);
            if (ui.msg_sel >= 0) ui.msg_sel = 0;  // keep detail view on the newest
            ui.msgov_on = true;
            ui.msgov_until = now + MSG_OVERLAY_MS;
            safe_copy(ui.msgov_from, sizeof(ui.msgov_from), m.s0);
            safe_copy(ui.msgov_text, sizeof(ui.msgov_text), m.s1);
            sleep_wake(ui.sleep, now);
            break;
        case CMD_ALERT:
            ui.alert_on = true;
            ui.alert_sticky = (m.ms == 0);
            ui.alert_until = now + m.ms;
            safe_copy(ui.alert_title, sizeof(ui.alert_title), m.s0);
            safe_copy(ui.alert_text, sizeof(ui.alert_text), m.s1);
            sleep_wake(ui.sleep, now);
            break;
        case CMD_PROGRESS:
            if (m.u8 == PROGRESS_HIDE) {
                if (!ui.prog_on) return;
                ui.prog_on = false;
            } else {
                uint8_t pct = clamp_pct(m.u8);
                bool same = ui.prog_on && ui.prog_pct == pct && strcmp(ui.prog_title, m.s0) == 0;
                ui.prog_on = true;
                ui.prog_pct = pct;
                ui.prog_last = now;
                safe_copy(ui.prog_title, sizeof(ui.prog_title), m.s0);
                sleep_wake(ui.sleep, now);
                if (same) return;
            }
            break;
        case CMD_BUTTON:
            handle_button(m, now);
            break;
        case CMD_STATUS:
            if (strcmp(ui.status, m.s1) == 0) return;
            safe_copy(ui.status, sizeof(ui.status), m.s1);
            break;
        default:
            return;
    }
    ui.dirty = true;
}

// Timers, sleep, and redraw decision. Called after every queue wake-up.
void service(uint32_t now) {
    if (ui.alert_on && !ui.alert_sticky && (int32_t)(now - ui.alert_until) >= 0) {
        ui.alert_on = false;
        ui.dirty = true;
    }
    if (ui.msgov_on && (int32_t)(now - ui.msgov_until) >= 0) {
        land_on_messages();
        ui.dirty = true;
    }
    if (ui.prog_on && (uint32_t)(now - ui.prog_last) >= PROGRESS_STALE_MS) {
        ui.prog_on = false;
        ui.dirty = true;
    }
    // A visible alert or progress bar keeps the screen awake.
    if (ui.alert_on || ui.prog_on) ui.sleep.last_activity = now;
    sleep_tick(ui.sleep, now);

    if (ui.sleep.on != ui.panel_on) {
        display.ssd1306_command(ui.sleep.on ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
        ui.panel_on = ui.sleep.on;
        if (ui.panel_on) ui.dirty = true;
    }
    if (!ui.panel_on) return;

    uint32_t since = now - ui.last_draw;
    bool overlay = ui.alert_on || ui.prog_on || ui.msgov_on;
    bool tick = !overlay && ui.page == PAGE_SYSTEM && since >= IDLE_TICK_MS;
    bool blink = !overlay && ui.wifi == WIFI_CONNECTING && since >= BLINK_MS;
    if ((ui.dirty && since >= MIN_FRAME_MS) || tick || blink) render(now);
}

// How long the task may sleep in xQueueReceive before service() is due.
uint32_t next_wait_ms(uint32_t now) {
    uint32_t wait = IDLE_TICK_MS;
    if (ui.wifi == WIFI_CONNECTING) wait = BLINK_MS;
    if (ui.dirty && ui.panel_on) {
        uint32_t since = now - ui.last_draw;
        uint32_t left = since >= MIN_FRAME_MS ? 0 : MIN_FRAME_MS - since;
        if (left < wait) wait = left;
    }
    if (ui.alert_on && !ui.alert_sticky) {
        int32_t left = (int32_t)(ui.alert_until - now);
        if (left < 0) left = 0;
        if ((uint32_t)left < wait) wait = left;
    }
    if (ui.msgov_on) {
        int32_t left = (int32_t)(ui.msgov_until - now);
        if (left < 0) left = 0;
        if ((uint32_t)left < wait) wait = left;
    }
    return wait;
}

// Apply queued updates (no drawing) until deadline_ms passes.
void pump_until(QueueHandle_t q, uint32_t deadline) {
    for (;;) {
        int32_t left = (int32_t)(deadline - millis());
        if (left <= 0) return;
        if (xQueueReceive(q, &rx_msg, pdMS_TO_TICKS(left)) == pdTRUE) apply(rx_msg, millis());
    }
}

// <= 1.2 s; keeps applying queued updates between frames so nothing is lost.
void boot_animation(QueueHandle_t q) {
    const int frames = 6;
    for (int f = 0; f < frames; f++) {
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        print_centered(14, PROJECT_NAME, 2);
        int r = 3 + f * 3;
        display.drawCircle(SCREEN_W / 2, 46, r, SSD1306_WHITE);
        if (f >= 2) display.drawCircle(SCREEN_W / 2, 46, r - 6, SSD1306_WHITE);
        display.display();
        pump_until(q, millis() + 150);
    }
    display.clearDisplay();
    print_centered(14, PROJECT_NAME, 2);
    print_centered(40, "LoRa messenger");
    display.display();
    pump_until(q, millis() + 300);
}

void display_task(void* param) {
    QueueHandle_t q = (QueueHandle_t)param;
    boot_animation(q);
    uint32_t now = millis();
    sleep_init(ui.sleep, now);
    ui.dirty = true;
    ui.last_draw = now - MIN_FRAME_MS;
    for (;;) {
        now = millis();
        uint32_t wait = next_wait_ms(now);
        if (xQueueReceive(q, &rx_msg, pdMS_TO_TICKS(wait)) == pdTRUE) {
            do {
                apply(rx_msg, millis());
            } while (xQueueReceive(q, &rx_msg, 0) == pdTRUE);  // coalesce bursts into one frame
        }
        service(millis());
    }
}

void ui_init() {
    memset(&ui, 0, sizeof(ui));
    ui.wifi = WIFI_DISCONNECTED;
    ui.lora = LORA_IDLE;
    ui.page = PAGE_STATUS;
    ui.msg_sel = -1;
    ui.panel_on = true;
    ring_init(ui.ring);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API (Contract 7)
// ---------------------------------------------------------------------------
void init_oled() {
    if (s_queue) return;  // already running
    if (!Wire.begin(OSDA, OSCL)) {
        Serial.println("[oled] I2C init failed, display disabled");
        return;
    }
    Wire.setTimeOut(50);
    Wire.beginTransmission(OLED_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("[oled] no panel at 0x3C, display disabled");
        return;
    }
    // periphBegin=false: Wire is already started on OSDA/OSCL.
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) {
        Serial.println("[oled] SSD1306 init/alloc failed, display disabled");
        return;
    }
    display.setTextWrap(false);
    display.cp437(true);
    ui_init();
    QueueHandle_t q = xQueueCreate(QUEUE_DEPTH, sizeof(OledMsg));
    if (!q) {
        Serial.println("[oled] queue alloc failed, display disabled");
        return;
    }
    if (xTaskCreatePinnedToCore(display_task, "oled", TASK_STACK, q, TASK_PRIO, nullptr, TASK_CORE) != pdPASS) {
        Serial.println("[oled] task create failed, display disabled");
        vQueueDelete(q);
        return;
    }
    s_queue = q;
}

void oled_set_wifi(WiFiStatus s, const char* ssid, const char* ip) {
    if (!api_ready()) return;
    OledMsg m;
    msg_init(m, CMD_WIFI);
    m.u8 = (uint8_t)s;
    m.b0 = ssid != nullptr;
    m.b1 = ip != nullptr;
    safe_copy(m.s0, sizeof(m.s0), ssid);
    safe_copy(m.s3, sizeof(m.s3), ip);
    post(m);
}

void oled_set_ap(const char* ssid, const char* pass, const char* ip) {
    if (!api_ready()) return;
    OledMsg m;
    msg_init(m, CMD_AP);
    safe_copy(m.s0, sizeof(m.s0), ssid);
    safe_copy(m.s2, sizeof(m.s2), pass);
    safe_copy(m.s3, sizeof(m.s3), ip);
    post(m);
}

void oled_set_wifi_rssi(int rssi) {
    if (!api_ready()) return;
    OledMsg m;
    msg_init(m, CMD_WIFI_RSSI);
    m.i0 = rssi;
    post(m);
}

void oled_set_lora(LoRaStatus s) {
    if (!api_ready()) return;
    OledMsg m;
    msg_init(m, CMD_LORA);
    m.u8 = (uint8_t)s;
    post(m);
}

void oled_lora_packet(bool tx, bool ok, int rssi, float snr) {
    if (!api_ready()) return;
    OledMsg m;
    msg_init(m, CMD_LORA_PKT);
    m.b0 = tx;
    m.b1 = ok;
    m.i0 = rssi;
    m.f = snr;
    post(m);
}

void oled_set_lora_params(long freq, int sf, long bw) {
    if (!api_ready()) return;
    OledMsg m;
    msg_init(m, CMD_LORA_PARAMS);
    m.i0 = (int32_t)freq;
    m.i1 = sf;
    m.i2 = (int32_t)bw;
    post(m);
}

void oled_message(const char* from, const char* text) {
    if (!api_ready()) return;
    OledMsg m;
    msg_init(m, CMD_MESSAGE);
    safe_copy(m.s0, sizeof(m.s0), from);
    safe_copy(m.s1, sizeof(m.s1), text);
    post(m);
}

void oled_alert(const char* title, const char* text, uint32_t ms) {
    if (!api_ready()) return;
    OledMsg m;
    msg_init(m, CMD_ALERT);
    m.ms = ms;
    safe_copy(m.s0, sizeof(m.s0), title);
    safe_copy(m.s1, sizeof(m.s1), text);
    post(m);
}

void oled_progress(const char* title, uint8_t pct) {
    if (!api_ready()) return;
    OledMsg m;
    msg_init(m, CMD_PROGRESS);
    m.u8 = pct;
    safe_copy(m.s0, sizeof(m.s0), title);
    post(m);
}

void oled_button(uint8_t btn, bool long_press) {
    if (!api_ready()) return;
    OledMsg m;
    msg_init(m, CMD_BUTTON);
    m.u8 = btn;
    m.b1 = long_press;
    post(m);
}

void oled_status(const char* text) {
    if (!api_ready()) return;
    OledMsg m;
    msg_init(m, CMD_STATUS);
    safe_copy(m.s1, sizeof(m.s1), text);
    post(m);
}
