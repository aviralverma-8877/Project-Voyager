// Pure layout and state logic for the OLED UI (owner: oled-display-engineer).
// No Arduino / ESP-IDF dependencies: compiled into the firmware by oled_display.cpp
// and unit-tested on the host by test/test_oled (env:native).
// Must stay C++11-compatible (the ESP32 toolchain builds with gnu++11).
#ifndef OLED_LAYOUT_H
#define OLED_LAYOUT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace oled_layout {

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
static const int SCREEN_W = 128;
static const int SCREEN_H = 64;
static const int CHAR_W = 6;   // Adafruit GFX built-in font at size 1 (5 px + 1 gap)
static const int CHAR_H = 8;

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

// strlen that treats nullptr as "".
inline size_t safe_len(const char* s) { return s ? strlen(s) : 0; }

// Null-safe bounded copy, always NUL-terminated (when n > 0).
inline void safe_copy(char* dst, size_t n, const char* src) {
    if (!dst || n == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// How many characters fit in px pixels at the given text size (size 0 treated as 1).
inline int chars_fit(int px, uint8_t size = 1) {
    if (size == 0) size = 1;
    if (px <= 0) return 0;
    return px / (CHAR_W * size);
}

// Width in pixels of a string at the given text size.
inline int text_width(const char* s, uint8_t size = 1) {
    if (size == 0) size = 1;
    return (int)safe_len(s) * CHAR_W * size;
}

// Copy src into out, shortening it to at most max_chars characters. When it has to
// cut, the tail is replaced by "..." (or as many dots as fit when max_chars < 3).
// Returns true when the text was truncated. out_size bounds the write as usual.
inline bool truncate_ellipsis(const char* src, char* out, size_t out_size, int max_chars) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (max_chars <= 0) return safe_len(src) > 0;
    size_t cap = out_size - 1;
    size_t limit = (size_t)max_chars < cap ? (size_t)max_chars : cap;
    size_t len = safe_len(src);
    if (len <= limit) { safe_copy(out, out_size, src); return false; }
    size_t dots = limit < 3 ? limit : 3;
    size_t keep = limit - dots;
    memcpy(out, src, keep);
    for (size_t i = 0; i < dots; i++) out[keep + i] = '.';
    out[keep + dots] = '\0';
    return true;
}

// Clamp an x coordinate so a span of width w starts on-screen (never negative).
inline int clamp_x(int x, int w, int screen_w = SCREEN_W) {
    if (w >= screen_w) return 0;
    if (x < 0) return 0;
    if (x + w > screen_w) return screen_w - w;
    return x;
}

// Left x that centres a span of text_px pixels; 0 when it does not fit.
inline int center_x(int text_px, int screen_w = SCREEN_W) {
    if (text_px >= screen_w || text_px < 0) return 0;
    return (screen_w - text_px) / 2;
}

// Word-wrap text into at most max_lines lines of at most max_chars characters.
// lines is a max_lines x stride char array (stride >= max_chars + 1).
// - breaks at spaces; '\n' forces a break; '\r' and '\t' are treated as spaces
// - a word longer than a line is hard-split
// - when text remains after the last line, that line ends in "..."
// Returns the number of lines written (0 for null/empty text).
inline int wrap_text(const char* text, int max_chars, int max_lines, char* lines, size_t stride) {
    if (!lines || stride < 2 || max_lines <= 0 || max_chars <= 0) return 0;
    if ((size_t)max_chars > stride - 1) max_chars = (int)stride - 1;
    for (int i = 0; i < max_lines; i++) lines[i * stride] = '\0';
    if (!text) return 0;

    const char* p = text;
    int n = 0;
    while (*p && n < max_lines) {
        // Skip leading spaces at the start of a wrapped line (not after '\n').
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p) break;
        if (*p == '\n') {               // empty line from consecutive newlines
            lines[n * stride] = '\0';
            n++;
            p++;
            continue;
        }
        // Find how much fits on this line.
        int len = 0;          // chars consumed
        int last_break = -1;  // length up to the last space we could break at
        while (p[len] && p[len] != '\n' && len < max_chars) {
            if (p[len] == ' ' || p[len] == '\t' || p[len] == '\r') last_break = len;
            len++;
        }
        int take = len;
        const char* next = p + len;
        if (p[len] && p[len] != '\n' && p[len] != ' ' && p[len] != '\t' && p[len] != '\r' &&
            last_break > 0) {
            // Line is full in the middle of a word: break at the last space.
            take = last_break;
            next = p + last_break;
        }
        // Copy and normalise whitespace; trim trailing spaces.
        char* dst = lines + n * stride;
        int w = 0;
        for (int i = 0; i < take; i++) {
            char c = p[i];
            dst[w++] = (c == '\t' || c == '\r') ? ' ' : c;
        }
        while (w > 0 && dst[w - 1] == ' ') w--;
        dst[w] = '\0';
        n++;
        p = next;
        if (*p == '\n') p++;
    }

    // Anything left (other than whitespace)? Mark the last line as cut.
    const char* rest = p;
    while (*rest == ' ' || *rest == '\t' || *rest == '\r' || *rest == '\n') rest++;
    if (*rest && n > 0) {
        char* last = lines + (n - 1) * stride;
        size_t l = strlen(last);
        size_t dots = max_chars < 3 ? (size_t)max_chars : 3;
        if (l + dots > (size_t)max_chars) l = (size_t)max_chars - dots;
        for (size_t i = 0; i < dots; i++) last[l + i] = '.';
        last[l + dots] = '\0';
    }
    return n;
}

// Hard character split (no word breaking), for values like SSIDs and passwords where
// every character matters. Overflow beyond max_lines ends the last line in "...".
// Returns the number of lines written.
inline int split_lines(const char* text, int max_chars, int max_lines, char* lines, size_t stride) {
    if (!lines || stride < 2 || max_lines <= 0 || max_chars <= 0) return 0;
    if ((size_t)max_chars > stride - 1) max_chars = (int)stride - 1;
    for (int i = 0; i < max_lines; i++) lines[i * stride] = '\0';
    size_t len = safe_len(text);
    if (len == 0) return 0;
    int n = 0;
    size_t pos = 0;
    while (pos < len && n < max_lines) {
        size_t remain = len - pos;
        bool last = (n == max_lines - 1);
        char* dst = lines + n * stride;
        if (last && remain > (size_t)max_chars) {
            truncate_ellipsis(text + pos, dst, stride, max_chars);
            pos = len;
        } else {
            size_t take = remain < (size_t)max_chars ? remain : (size_t)max_chars;
            memcpy(dst, text + pos, take);
            dst[take] = '\0';
            pos += take;
        }
        n++;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------

// 255 is the "hide" sentinel for oled_progress.
static const uint8_t PROGRESS_HIDE = 255;

inline uint8_t clamp_pct(int pct) {
    if (pct < 0) return 0;
    if (pct > 100) return 100;
    return (uint8_t)pct;
}

// Filled width inside a progress bar whose inner width is inner_px.
inline int progress_fill(int inner_px, int pct) {
    if (inner_px <= 0) return 0;
    return inner_px * clamp_pct(pct) / 100;
}

// WiFi RSSI (dBm) to 0..4 bars. 0 dBm or above is "no reading" -> 0 bars.
inline uint8_t rssi_to_bars(int rssi) {
    if (rssi >= 0) return 0;
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

// "HH:MM:SS", or "Nd HH:MM:SS" once a day has passed.
inline void format_uptime(uint64_t seconds, char* out, size_t n) {
    if (!out || n == 0) return;
    unsigned long d = (unsigned long)(seconds / 86400ULL);
    unsigned h = (unsigned)((seconds / 3600ULL) % 24ULL);
    unsigned m = (unsigned)((seconds / 60ULL) % 60ULL);
    unsigned s = (unsigned)(seconds % 60ULL);
    if (d > 0) snprintf(out, n, "%lud %02u:%02u:%02u", d, h, m, s);
    else       snprintf(out, n, "%02u:%02u:%02u", h, m, s);
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------
enum Page : uint8_t { PAGE_STATUS = 0, PAGE_MESSAGES, PAGE_LORA, PAGE_NETWORK, PAGE_SYSTEM, PAGE_COUNT };

inline uint8_t next_page(uint8_t cur, uint8_t count = PAGE_COUNT) {
    if (count == 0) return 0;
    return (uint8_t)((cur + 1u) % count);
}

// ---------------------------------------------------------------------------
// Sleep / wake / button state machine
// ---------------------------------------------------------------------------
static const uint32_t SLEEP_TIMEOUT_MS = 60000;

struct SleepState {
    bool on;
    uint32_t last_activity;
};

inline void sleep_init(SleepState& s, uint32_t now) { s.on = true; s.last_activity = now; }

// What the display task should do with a button press.
enum ButtonAction : uint8_t {
    BTN_ACT_NONE = 0,     // screen was on, press has no display meaning (e.g. BTN1 off Messages)
    BTN_ACT_WAKE,         // screen was off: it is now on, press consumed
    BTN_ACT_SCREEN_OFF,   // BTN2 long press turned the screen off
    BTN_ACT_NEXT_PAGE,    // BTN2 short press while on
    BTN_ACT_BTN1,         // BTN1 short press while on (page-specific action)
};

inline ButtonAction sleep_button(SleepState& s, uint8_t btn, bool long_press, uint32_t now) {
    s.last_activity = now;
    if (btn == 2 && long_press) {
        s.on = !s.on;
        return s.on ? BTN_ACT_WAKE : BTN_ACT_SCREEN_OFF;
    }
    if (!s.on) { s.on = true; return BTN_ACT_WAKE; }   // the waking press does nothing else
    if (btn == 2) return BTN_ACT_NEXT_PAGE;
    if (btn == 1 && !long_press) return BTN_ACT_BTN1;
    return BTN_ACT_NONE;
}

// Message / alert / progress: wake the screen and restart the idle timer.
// Returns true when the screen was off.
inline bool sleep_wake(SleepState& s, uint32_t now) {
    bool was_off = !s.on;
    s.on = true;
    s.last_activity = now;
    return was_off;
}

// Periodic check. Returns true exactly when the screen should go off now.
// Wrap-safe (unsigned subtraction).
inline bool sleep_tick(SleepState& s, uint32_t now, uint32_t timeout = SLEEP_TIMEOUT_MS) {
    if (!s.on) return false;
    if ((uint32_t)(now - s.last_activity) >= timeout) { s.on = false; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Received-message ring (newest first)
// ---------------------------------------------------------------------------
static const int MSG_RING_SIZE = 5;
static const int MSG_FROM_LEN = 33;   // 32 chars + NUL (username max is 32)
static const int MSG_TEXT_LEN = 97;   // 96 chars + NUL: more than fits on the screen

struct MsgRing {
    char from[MSG_RING_SIZE][MSG_FROM_LEN];
    char text[MSG_RING_SIZE][MSG_TEXT_LEN];
    uint8_t head;    // slot the next push writes
    uint8_t count;   // 0..MSG_RING_SIZE
    uint32_t total;  // messages ever pushed
};

inline void ring_init(MsgRing& r) { memset(&r, 0, sizeof(r)); }

inline void ring_push(MsgRing& r, const char* from, const char* text) {
    safe_copy(r.from[r.head], MSG_FROM_LEN, from);
    safe_copy(r.text[r.head], MSG_TEXT_LEN, text);
    r.head = (uint8_t)((r.head + 1) % MSG_RING_SIZE);
    if (r.count < MSG_RING_SIZE) r.count++;
    r.total++;
}

// i = 0 is the newest. Returns -1 when i is out of range.
inline int ring_slot(const MsgRing& r, int i) {
    if (i < 0 || i >= r.count) return -1;
    return (r.head + MSG_RING_SIZE - 1 - i) % MSG_RING_SIZE;
}
inline const char* ring_from(const MsgRing& r, int i) { int k = ring_slot(r, i); return k < 0 ? "" : r.from[k]; }
inline const char* ring_text(const MsgRing& r, int i) { int k = ring_slot(r, i); return k < 0 ? "" : r.text[k]; }

// Messages page selection: -1 = list view, 0..count-1 = detail view of that message.
// BTN1 short press walks list -> newest -> ... -> oldest -> list.
inline int8_t msg_select_next(int8_t sel, uint8_t count) {
    if (count == 0) return -1;
    int next = sel + 1;
    if (next >= count) return -1;
    return (int8_t)next;
}

}  // namespace oled_layout

#endif  // OLED_LAYOUT_H
