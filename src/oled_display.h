// OLED UI (owner: oled-display-engineer). Implements Contract 7.
//
// Every call below is non-blocking and safe from any task (not from ISRs: ISR calls
// are ignored). Arguments are copied into a fixed-size queue message, so callers'
// strings need not outlive the call. All I2C drawing happens on one display task.
// If the queue is full the update is dropped. If no panel was found at boot, or
// memory ran out, every call is a no-op.
#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include <stdint.h>

// Status indicators (kept here for callers).
enum WiFiStatus {
    WIFI_DISCONNECTED,
    WIFI_CONNECTING,
    WIFI_CONNECTED_AP,
    WIFI_CONNECTED_STA
};

enum LoRaStatus {
    LORA_IDLE,
    LORA_TRANSMITTING,
    LORA_RECEIVING,
    LORA_ERROR
};

// Probes the panel at 0x3C, then starts the display task (boot animation <= 1.5 s
// runs on that task, so this returns quickly). No panel / no memory: API is a no-op.
void init_oled();

void oled_set_wifi(WiFiStatus s, const char* ssid, const char* ip);
void oled_set_ap(const char* ssid, const char* pass, const char* ip);  // AP details for the Network page
void oled_set_wifi_rssi(int rssi);
void oled_set_lora(LoRaStatus s);
void oled_lora_packet(bool tx, bool ok, int rssi, float snr);  // counters + last RSSI/SNR (rssi/snr ignored for tx)
void oled_message(const char* from, const char* text);         // overlay, then Messages page (last 5)
void oled_alert(const char* title, const char* text, uint32_t ms);  // priority overlay; ms==0: until a button press
void oled_progress(const char* title, uint8_t pct);            // pct 0-100 shows it (clamped), 255 hides it
void oled_button(uint8_t btn, bool long_press);                // input from core's button task
void oled_status(const char* text);                            // one-line status on the Status page

// Contract addition (oled-display-engineer): radio parameters for the LoRa page.
// freq in Hz (e.g. 510000000), sf 6-12, bw in Hz (e.g. 125000).
void oled_set_lora_params(long freq, int sf, long bw);

#endif  // OLED_DISPLAY_H
