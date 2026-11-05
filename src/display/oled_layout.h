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

// In place, for text from the network or a LoRa peer: the built-in font has one glyph
// per byte, so each UTF-8 multi-byte sequence (or stray high byte) becomes a single
// '?', and control characters other than '\n' become ' '. Keeps width math honest.
inline void ascii_fold(char* s) {
    if (!s) return;
    const unsigned char* r = (const unsigned char*)s;
    char* w = s;
    bool in_seq = false;
    while (*r) {
        unsigned char c = *r++;
        if (c >= 0xC0) { *w++ = '?'; in_seq = true; continue; }       // UTF-8 lead byte
        if (c >= 0x80) { if (!in_seq) *w++ = '?'; continue; }         // continuation / stray
        in_seq = false;
        *w++ = (c < 0x20 && c != '\n') || c == 0x7F ? ' ' : (char)c;
    }
    *w = '\0';
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
// Screen layout for a two-colour panel
//
// Common 128x64 SSD1306 modules are yellow in rows 0..15 and blue in rows 16..63.
// The header (title, icons, separator) and overlay title bars stay in rows
// 0..OLED_HEADER_H-1; page bodies, overlay bodies and the page dots draw only at
// y >= OLED_HEADER_H. For a panel with a different split, change OLED_HEADER_H only
// (the static_asserts below check that everything still fits).
// ---------------------------------------------------------------------------
static const int OLED_HEADER_H = 16;

// A rectangle: rows y..y+h-1, columns x..x+w-1.
struct Box { int x, y, w, h; };
inline int box_bottom(const Box& b) { return b.y + b.h - 1; }
inline int box_right(const Box& b) { return b.x + b.w - 1; }
inline bool box_in_header(const Box& b) { return b.y >= 0 && box_bottom(b) < OLED_HEADER_H; }
inline bool box_in_body(const Box& b) { return b.y >= OLED_HEADER_H && box_bottom(b) < SCREEN_H; }
inline bool box_on_screen_x(const Box& b) { return b.x >= 0 && box_right(b) < SCREEN_W; }
inline bool boxes_overlap(const Box& a, const Box& b) {
    return a.x <= box_right(b) && b.x <= box_right(a) && a.y <= box_bottom(b) && b.y <= box_bottom(a);
}

// Header band. Titles are drawn upper-case at text size 2: the glyph cell is 12x16,
// and upper-case letters, digits and most punctuation use only font rows 0..6
// (14 px at size 2). Only ',' g p q y use font row 7, so upper-casing keeps the
// ink off the separator row.
static const int HEADER_SEP_Y = OLED_HEADER_H - 1;                        // 15
static const int BAND_INK_H2 = 7 * 2;                                     // size-2 caps
static const int BAND_INK_H1 = 7;                                         // size-1 caps
static const int BAND_TEXT_Y2 = (HEADER_SEP_Y - BAND_INK_H2) / 2;         // 0  -> ink 0..13
static const int BAND_TEXT_Y1 = (HEADER_SEP_Y - BAND_INK_H1) / 2;         // 4  -> ink 4..10

// Header icons, right-aligned; art occupies rows ICON_TOP_Y..ICON_BASE_Y.
static const int ICON_BASE_Y = HEADER_SEP_Y - 2;                          // 13
static const int ICON_TOP_Y = ICON_BASE_Y - 11;                           // 2 (12 rows)
static const int ICON_WIFI_X = 86, ICON_WIFI_W = 13;   // arcs r=6,4,2 around (92, 13)
static const int ICON_LORA_X = 102, ICON_LORA_W = 9;   // mast + arrow
static const int ICON_BARS_X = 116, ICON_BARS_W = 12;  // 4 bars (STA) or "AP" (AP mode)
static const int HEADER_TITLE_PX = ICON_WIFI_X - 2;                       // 84
static const int HEADER_TITLE_MAX2 = HEADER_TITLE_PX / (CHAR_W * 2);      // 7 chars at size 2
static const int HEADER_TITLE_MAX1 = HEADER_TITLE_PX / CHAR_W;            // 14 chars at size 1

// Page body: size-1 lines, then the page dots on the bottom rows.
static const int DOTS_H = 3;
static const int DOTS_Y = SCREEN_H - DOTS_H;                              // 61..63
static const int DOTS_SPACING = 6;
static const int CONTENT_Y = OLED_HEADER_H + 1;                           // 17 (row 16 blank)
static const int LINE_STEP = 9;
static const int CONTENT_LINES = (DOTS_Y - CHAR_H - CONTENT_Y) / LINE_STEP + 1;  // 5

// Alert overlay: inverted title band, framed body with wrapped text.
static const int OVERLAY_MARGIN = 4;
static const int ALERT_TEXT_Y = OLED_HEADER_H + 3;                        // 19
static const int ALERT_LINES = (SCREEN_H - 1 - CHAR_H - ALERT_TEXT_Y) / LINE_STEP + 1;  // 5

// Progress overlay ("Hold for SOS", "RX <file>"): title in band, bar + percent below.
static const int PROG_BAR_Y = OLED_HEADER_H + 8;                          // 24..35
static const int PROG_BAR_H = 12;
static const int PROG_PCT_Y = PROG_BAR_Y + PROG_BAR_H + 6;                // 42..57 (size 2)

// New-message overlay: "NEW MSG" in band, sender, two lines of text.
static const int MSGOV_FROM_Y2 = OLED_HEADER_H + 4;                       // 20..35 (size 2)
static const int MSGOV_FROM_Y1 = OLED_HEADER_H + 8;                       // 24..31 (size 1)
static const int MSGOV_TEXT_Y = OLED_HEADER_H + 26;                       // 42, 51
static const int MSGOV_TEXT_LINES = 2;

// Boot splash: name in band, expanding rings centred in the body.
static const int SPLASH_FRAMES = 6;
static const int SPLASH_CX = SCREEN_W / 2;
static const int SPLASH_CY = OLED_HEADER_H + (SCREEN_H - OLED_HEADER_H) / 2;   // 40
static const int SPLASH_TAG_Y = SPLASH_CY - CHAR_H / 2;                          // 36

static_assert(OLED_HEADER_H >= 14 && OLED_HEADER_H <= SCREEN_H - 40, "header band size");
static_assert(BAND_TEXT_Y2 >= 0 && BAND_TEXT_Y2 + 2 * CHAR_H - 1 <= HEADER_SEP_Y, "size-2 title fits band");
static_assert(ICON_TOP_Y >= 0, "icons fit band");
static_assert(CONTENT_LINES >= 4, "at least 4 body lines");
static_assert(PROG_PCT_Y + 2 * CHAR_H <= DOTS_Y + DOTS_H, "progress fits body");

// Text size for a band title: 2 when it fits avail_px, else 1.
inline uint8_t band_text_size(const char* s, int avail_px) { return text_width(s, 2) <= avail_px ? 2 : 1; }
inline int band_text_y(uint8_t size) { return size >= 2 ? BAND_TEXT_Y2 : BAND_TEXT_Y1; }

// Upper-case ASCII in place (band titles, see above).
inline void ascii_upper(char* s) {
    if (!s) return;
    for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
}

// Element boxes (glyph cells, not just ink). The drawing code positions with these.
inline Box header_sep_box() { Box b = {0, HEADER_SEP_Y, SCREEN_W, 1}; return b; }
inline Box header_title_box(uint8_t size) {
    Box b = {0, band_text_y(size), size >= 2 ? HEADER_TITLE_MAX2 * CHAR_W * 2 : HEADER_TITLE_MAX1 * CHAR_W,
             CHAR_H * (size >= 2 ? 2 : 1)};
    return b;
}
inline Box icon_wifi_box() { Box b = {ICON_WIFI_X, ICON_TOP_Y, ICON_WIFI_W, ICON_BASE_Y - ICON_TOP_Y + 1}; return b; }
inline Box icon_lora_box() { Box b = {ICON_LORA_X, ICON_TOP_Y, ICON_LORA_W, ICON_BASE_Y - ICON_TOP_Y + 1}; return b; }
inline Box icon_bars_box() { Box b = {ICON_BARS_X, ICON_TOP_Y, ICON_BARS_W, ICON_BASE_Y - ICON_TOP_Y + 1}; return b; }
inline Box content_line_box(int i) { Box b = {0, CONTENT_Y + i * LINE_STEP, SCREEN_W, CHAR_H}; return b; }
inline int dots_x0(uint8_t count = PAGE_COUNT) {
    int total = (count - 1) * DOTS_SPACING + 3;
    return (SCREEN_W - total) / 2 + 1;   // centre of the first dot
}
inline Box dots_box(uint8_t count = PAGE_COUNT) {
    Box b = {dots_x0(count) - 1, DOTS_Y, (count - 1) * DOTS_SPACING + 3, DOTS_H};
    return b;
}
inline Box alert_band_box() { Box b = {0, 0, SCREEN_W, OLED_HEADER_H}; return b; }
inline Box alert_frame_box() { Box b = {0, OLED_HEADER_H, SCREEN_W, SCREEN_H - OLED_HEADER_H}; return b; }
inline Box alert_line_box(int i) {
    Box b = {OVERLAY_MARGIN, ALERT_TEXT_Y + i * LINE_STEP, SCREEN_W - 2 * OVERLAY_MARGIN, CHAR_H};
    return b;
}
inline Box progress_bar_box() { Box b = {2 * OVERLAY_MARGIN, PROG_BAR_Y, SCREEN_W - 4 * OVERLAY_MARGIN, PROG_BAR_H}; return b; }
inline Box progress_pct_box() { Box b = {0, PROG_PCT_Y, SCREEN_W, 2 * CHAR_H}; return b; }
inline Box msgov_from_box(uint8_t size) {
    Box b = {0, size >= 2 ? MSGOV_FROM_Y2 : MSGOV_FROM_Y1, SCREEN_W, CHAR_H * (size >= 2 ? 2 : 1)};
    return b;
}
inline Box msgov_text_box(int i) {
    Box b = {OVERLAY_MARGIN, MSGOV_TEXT_Y + i * LINE_STEP, SCREEN_W - 2 * OVERLAY_MARGIN, CHAR_H};
    return b;
}
inline int splash_ring_r(int frame) { return 3 + frame * 3; }
inline Box splash_ring_box(int frame) {
    int r = splash_ring_r(frame);
    Box b = {SPLASH_CX - r, SPLASH_CY - r, 2 * r + 1, 2 * r + 1};
    return b;
}
inline Box splash_tag_box() { Box b = {0, SPLASH_TAG_Y, SCREEN_W, CHAR_H}; return b; }

// ---------------------------------------------------------------------------
// Sleep / wake / button state machine
// ---------------------------------------------------------------------------
static const uint32_t SLEEP_TIMEOUT_MS = 60000;

// Core shows the "Hold for SOS" progress bar on the BTN1 press-down edge, before the
// release sends oled_button(1,false). If that progress update is what woke the screen,
// the BTN1 short release that follows (< SOS hold time) is still the waking press.
static const uint32_t HOLD_WAKE_WINDOW_MS = 2500;

struct SleepState {
    bool on;
    uint32_t last_activity;
    bool hold_wake;         // a progress update woke the screen at hold_wake_at
    uint32_t hold_wake_at;
};

inline void sleep_init(SleepState& s, uint32_t now) {
    s.on = true;
    s.last_activity = now;
    s.hold_wake = false;
    s.hold_wake_at = 0;
}

// What the display task should do with a button press.
enum ButtonAction : uint8_t {
    BTN_ACT_NONE = 0,     // screen was on, press has no display meaning (e.g. BTN1 off Messages)
    BTN_ACT_WAKE,         // screen was off: it is now on, press consumed
    BTN_ACT_SCREEN_OFF,   // BTN2 long press turned the screen off
    BTN_ACT_NEXT_PAGE,    // BTN2 short press while on
    BTN_ACT_BTN1,         // BTN1 short press while on (page-specific action)
};

inline ButtonAction sleep_button(SleepState& s, uint8_t btn, bool long_press, uint32_t now) {
    bool hold_woke = s.hold_wake && btn == 1 && !long_press &&
                     (uint32_t)(now - s.hold_wake_at) <= HOLD_WAKE_WINDOW_MS;
    s.hold_wake = false;
    s.last_activity = now;
    if (hold_woke) { s.on = true; return BTN_ACT_WAKE; }
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

// Progress update: wakes like sleep_wake, and remembers that it did so (see
// HOLD_WAKE_WINDOW_MS) so the BTN1 release behind it only counts as the wake.
inline bool sleep_wake_progress(SleepState& s, uint32_t now) {
    bool was_off = sleep_wake(s, now);
    if (was_off) { s.hold_wake = true; s.hold_wake_at = now; }
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
