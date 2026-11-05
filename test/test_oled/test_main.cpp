// L2 tests for the OLED layout/state logic (src/oled_layout.h). Owner: oled-display-engineer.
// Run: ~/.platformio/penv/bin/pio test -e native -f test_oled
#include <unity.h>
#include <string.h>
#include <stdio.h>
#include <string>
#include "display/oled_layout.h"

using namespace oled_layout;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- strings
static void test_safe_copy_null_and_bounds() {
    char b[4] = "xyz";
    safe_copy(b, sizeof(b), nullptr);
    TEST_ASSERT_EQUAL_STRING("", b);
    safe_copy(b, sizeof(b), "abcdef");
    TEST_ASSERT_EQUAL_STRING("abc", b);
    safe_copy(b, 0, "q");  // must not write
    TEST_ASSERT_EQUAL_STRING("abc", b);
    TEST_ASSERT_EQUAL_UINT(0, safe_len(nullptr));
}

static void test_chars_fit_and_width() {
    TEST_ASSERT_EQUAL_INT(21, chars_fit(128));
    TEST_ASSERT_EQUAL_INT(10, chars_fit(128, 2));
    TEST_ASSERT_EQUAL_INT(21, chars_fit(128, 0));  // size 0 treated as 1
    TEST_ASSERT_EQUAL_INT(0, chars_fit(-5));
    TEST_ASSERT_EQUAL_INT(42, text_width("Voyager"));
    TEST_ASSERT_EQUAL_INT(84, text_width("Voyager", 2));
    TEST_ASSERT_EQUAL_INT(0, text_width(nullptr));
}

static void test_truncate_ellipsis() {
    char out[32];
    TEST_ASSERT_FALSE(truncate_ellipsis("short", out, sizeof(out), 21));
    TEST_ASSERT_EQUAL_STRING("short", out);
    TEST_ASSERT_FALSE(truncate_ellipsis("exactly-ten", out, sizeof(out), 11));
    TEST_ASSERT_EQUAL_STRING("exactly-ten", out);
    TEST_ASSERT_TRUE(truncate_ellipsis("exactly-ten", out, sizeof(out), 10));
    TEST_ASSERT_EQUAL_STRING("exactly...", out);
    TEST_ASSERT_EQUAL_UINT(10, strlen(out));
    // 32-char SSID on a 21-char line
    TEST_ASSERT_TRUE(truncate_ellipsis("ABCDEFGHIJKLMNOPQRSTUVWXYZ012345", out, sizeof(out), 21));
    TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKLMNOPQR...", out);
    // tiny widths
    TEST_ASSERT_TRUE(truncate_ellipsis("abcdef", out, sizeof(out), 2));
    TEST_ASSERT_EQUAL_STRING("..", out);
    TEST_ASSERT_TRUE(truncate_ellipsis("abcdef", out, sizeof(out), 0));
    TEST_ASSERT_EQUAL_STRING("", out);
    // null / empty
    TEST_ASSERT_FALSE(truncate_ellipsis(nullptr, out, sizeof(out), 10));
    TEST_ASSERT_EQUAL_STRING("", out);
    // output buffer smaller than max_chars: bounded by out_size
    char small[6];
    TEST_ASSERT_TRUE(truncate_ellipsis("abcdefghij", small, sizeof(small), 21));
    TEST_ASSERT_EQUAL_STRING("ab...", small);
}

static void test_center_and_clamp_x() {
    TEST_ASSERT_EQUAL_INT(43, center_x(42));
    TEST_ASSERT_EQUAL_INT(0, center_x(128));
    TEST_ASSERT_EQUAL_INT(0, center_x(500));
    TEST_ASSERT_EQUAL_INT(0, center_x(-1));
    TEST_ASSERT_EQUAL_INT(64, center_x(0));
    TEST_ASSERT_EQUAL_INT(0, clamp_x(-10, 20));
    TEST_ASSERT_EQUAL_INT(108, clamp_x(120, 20));
    TEST_ASSERT_EQUAL_INT(0, clamp_x(50, 200));
    TEST_ASSERT_EQUAL_INT(30, clamp_x(30, 20));
}

typedef char Lines[6][22];

static void test_wrap_basic_words() {
    Lines l;
    int n = wrap_text("hello world from voyager", 10, 6, &l[0][0], sizeof(l[0]));
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("hello", l[0]);
    TEST_ASSERT_EQUAL_STRING("world from", l[1]);
    TEST_ASSERT_EQUAL_STRING("voyager", l[2]);
}

static void test_wrap_empty_and_null() {
    Lines l;
    l[0][0] = 'x';
    TEST_ASSERT_EQUAL_INT(0, wrap_text("", 21, 4, &l[0][0], sizeof(l[0])));
    TEST_ASSERT_EQUAL_STRING("", l[0]);
    TEST_ASSERT_EQUAL_INT(0, wrap_text(nullptr, 21, 4, &l[0][0], sizeof(l[0])));
    TEST_ASSERT_EQUAL_INT(0, wrap_text("   ", 21, 4, &l[0][0], sizeof(l[0])));
    TEST_ASSERT_EQUAL_INT(0, wrap_text("abc", 0, 4, &l[0][0], sizeof(l[0])));
}

static void test_wrap_long_word_hard_split() {
    Lines l;
    int n = wrap_text("abcdefghijklmnopqrstuvwxyz", 10, 6, &l[0][0], sizeof(l[0]));
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("abcdefghij", l[0]);
    TEST_ASSERT_EQUAL_STRING("klmnopqrst", l[1]);
    TEST_ASSERT_EQUAL_STRING("uvwxyz", l[2]);
}

static void test_wrap_overflow_gets_ellipsis() {
    Lines l;
    int n = wrap_text("one two three four five six seven", 9, 2, &l[0][0], sizeof(l[0]));
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("one two", l[0]);
    TEST_ASSERT_EQUAL_STRING("three...", l[1]);
    // last line full: dots replace its tail, length stays <= max_chars
    n = wrap_text("abcdefghijklmnop", 5, 2, &l[0][0], sizeof(l[0]));
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("abcde", l[0]);
    TEST_ASSERT_EQUAL_STRING("fg...", l[1]);
}

static void test_wrap_newlines_and_tabs() {
    Lines l;
    int n = wrap_text("a\nb\tc\r\n\nd", 21, 6, &l[0][0], sizeof(l[0]));
    TEST_ASSERT_EQUAL_INT(4, n);
    TEST_ASSERT_EQUAL_STRING("a", l[0]);
    TEST_ASSERT_EQUAL_STRING("b c", l[1]);
    TEST_ASSERT_EQUAL_STRING("", l[2]);
    TEST_ASSERT_EQUAL_STRING("d", l[3]);
}

static void test_wrap_clamps_to_stride() {
    char l[2][5];  // stride 5 -> at most 4 chars per line
    int n = wrap_text("abcdefgh", 21, 2, &l[0][0], sizeof(l[0]));
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("abcd", l[0]);
    TEST_ASSERT_EQUAL_STRING("efgh", l[1]);
}

static void test_split_lines() {
    Lines l;
    int n = split_lines("SSID ABCDEFGHIJKLMNOPQRSTUVWXYZ012345", 21, 2, &l[0][0], sizeof(l[0]));
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("SSID ABCDEFGHIJKLMNOP", l[0]);
    TEST_ASSERT_EQUAL_STRING("QRSTUVWXYZ012345", l[1]);
    // 63-char password + label overflows 2 lines -> ellipsis
    std::string pw = "Pass " + std::string(63, 'p');
    n = split_lines(pw.c_str(), 21, 2, &l[0][0], sizeof(l[0]));
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_UINT(21, strlen(l[1]));
    TEST_ASSERT_EQUAL_STRING("...", l[1] + 18);
    TEST_ASSERT_EQUAL_INT(0, split_lines(nullptr, 21, 2, &l[0][0], sizeof(l[0])));
    TEST_ASSERT_EQUAL_INT(1, split_lines("x", 21, 2, &l[0][0], sizeof(l[0])));
}

// ---------------------------------------------------------------- numbers
static void test_progress_width() {
    TEST_ASSERT_EQUAL_INT(0, progress_fill(110, 0));
    TEST_ASSERT_EQUAL_INT(55, progress_fill(110, 50));
    TEST_ASSERT_EQUAL_INT(110, progress_fill(110, 100));
    TEST_ASSERT_EQUAL_INT(110, progress_fill(110, 150));  // clamped
    TEST_ASSERT_EQUAL_INT(0, progress_fill(110, -5));
    TEST_ASSERT_EQUAL_INT(0, progress_fill(0, 50));
    TEST_ASSERT_EQUAL_INT(0, progress_fill(-3, 50));
    TEST_ASSERT_EQUAL_UINT8(100, clamp_pct(150));
    TEST_ASSERT_EQUAL_UINT8(100, clamp_pct(254));
    TEST_ASSERT_EQUAL_UINT8(0, clamp_pct(-1));
}

static void test_rssi_bars() {
    TEST_ASSERT_EQUAL_UINT8(0, rssi_to_bars(0));     // no reading
    TEST_ASSERT_EQUAL_UINT8(0, rssi_to_bars(10));
    TEST_ASSERT_EQUAL_UINT8(4, rssi_to_bars(-30));
    TEST_ASSERT_EQUAL_UINT8(4, rssi_to_bars(-55));
    TEST_ASSERT_EQUAL_UINT8(3, rssi_to_bars(-56));
    TEST_ASSERT_EQUAL_UINT8(3, rssi_to_bars(-65));
    TEST_ASSERT_EQUAL_UINT8(2, rssi_to_bars(-75));
    TEST_ASSERT_EQUAL_UINT8(1, rssi_to_bars(-85));
    TEST_ASSERT_EQUAL_UINT8(0, rssi_to_bars(-86));
    TEST_ASSERT_EQUAL_UINT8(0, rssi_to_bars(-120));
}

static void test_uptime_format() {
    char b[24];
    format_uptime(0, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("00:00:00", b);
    format_uptime(3661, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("01:01:01", b);
    format_uptime(86399, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("23:59:59", b);
    format_uptime(86400 + 59, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("1d 00:00:59", b);
    format_uptime(60ULL * 86400ULL, b, sizeof(b));  // past the 49.7-day millis() wrap
    TEST_ASSERT_EQUAL_STRING("60d 00:00:00", b);
    char tiny[4];
    format_uptime(3661, tiny, sizeof(tiny));  // truncated, still terminated
    TEST_ASSERT_EQUAL_STRING("01:", tiny);
}

// ---------------------------------------------------------------- pages
static void test_page_cycling() {
    TEST_ASSERT_EQUAL_UINT8(PAGE_MESSAGES, next_page(PAGE_STATUS));
    TEST_ASSERT_EQUAL_UINT8(PAGE_SYSTEM, next_page(PAGE_NETWORK));
    TEST_ASSERT_EQUAL_UINT8(PAGE_STATUS, next_page(PAGE_SYSTEM));
    uint8_t p = PAGE_STATUS;
    for (int i = 0; i < PAGE_COUNT; i++) p = next_page(p);
    TEST_ASSERT_EQUAL_UINT8(PAGE_STATUS, p);
    TEST_ASSERT_EQUAL_UINT8(0, next_page(3, 0));
}

// ---------------------------------------------------------------- sleep/wake
static void test_sleep_after_60s_idle() {
    SleepState s;
    sleep_init(s, 1000);
    TEST_ASSERT_FALSE(sleep_tick(s, 1000 + 59999));
    TEST_ASSERT_TRUE(s.on);
    TEST_ASSERT_TRUE(sleep_tick(s, 1000 + 60000));
    TEST_ASSERT_FALSE(s.on);
    TEST_ASSERT_FALSE(sleep_tick(s, 1000 + 70000));  // reports the transition once
}

static void test_wake_press_is_consumed() {
    SleepState s;
    sleep_init(s, 0);
    sleep_tick(s, 60000);
    TEST_ASSERT_FALSE(s.on);
    TEST_ASSERT_EQUAL_UINT8(BTN_ACT_WAKE, sleep_button(s, 2, false, 61000));  // no page change
    TEST_ASSERT_TRUE(s.on);
    TEST_ASSERT_EQUAL_UINT8(BTN_ACT_NEXT_PAGE, sleep_button(s, 2, false, 61500));
    sleep_tick(s, 61500 + 60000);
    TEST_ASSERT_EQUAL_UINT8(BTN_ACT_WAKE, sleep_button(s, 1, false, 200000));  // BTN1 also only wakes
    TEST_ASSERT_EQUAL_UINT8(BTN_ACT_BTN1, sleep_button(s, 1, false, 200100));
}

static void test_long_press_toggles() {
    SleepState s;
    sleep_init(s, 0);
    TEST_ASSERT_EQUAL_UINT8(BTN_ACT_SCREEN_OFF, sleep_button(s, 2, true, 10));
    TEST_ASSERT_FALSE(s.on);
    TEST_ASSERT_FALSE(sleep_tick(s, 100000));
    TEST_ASSERT_EQUAL_UINT8(BTN_ACT_WAKE, sleep_button(s, 2, true, 100001));
    TEST_ASSERT_TRUE(s.on);
    TEST_ASSERT_EQUAL_UINT8(BTN_ACT_NONE, sleep_button(s, 1, true, 100002));  // BTN1 long = core's SOS
}

static void test_activity_restarts_timer_and_wake() {
    SleepState s;
    sleep_init(s, 0);
    sleep_button(s, 2, false, 50000);
    TEST_ASSERT_FALSE(sleep_tick(s, 100000));  // 50 s since the press
    TEST_ASSERT_TRUE(sleep_tick(s, 110000));
    TEST_ASSERT_TRUE(sleep_wake(s, 120000));   // alert/message wakes; was off
    TEST_ASSERT_TRUE(s.on);
    TEST_ASSERT_FALSE(sleep_wake(s, 120001));  // already on
}

static void test_sleep_millis_wrap() {
    SleepState s;
    sleep_init(s, 0xFFFFF000u);
    TEST_ASSERT_FALSE(sleep_tick(s, 0x00001000u));  // 8 s later, across the wrap
    TEST_ASSERT_TRUE(sleep_tick(s, 0xFFFFF000u + 60000u));
}

// ---------------------------------------------------------------- message ring
static void test_ring_newest_first_and_capacity() {
    MsgRing r;
    ring_init(r);
    TEST_ASSERT_EQUAL_UINT8(0, r.count);
    TEST_ASSERT_EQUAL_STRING("", ring_from(r, 0));
    char from[8], text[8];
    for (int i = 1; i <= 7; i++) {
        snprintf(from, sizeof(from), "u%d", i);
        snprintf(text, sizeof(text), "m%d", i);
        ring_push(r, from, text);
    }
    TEST_ASSERT_EQUAL_UINT8(5, r.count);
    TEST_ASSERT_EQUAL_UINT32(7, r.total);
    TEST_ASSERT_EQUAL_STRING("u7", ring_from(r, 0));
    TEST_ASSERT_EQUAL_STRING("m7", ring_text(r, 0));
    TEST_ASSERT_EQUAL_STRING("u3", ring_from(r, 4));
    TEST_ASSERT_EQUAL_STRING("", ring_from(r, 5));
    TEST_ASSERT_EQUAL_STRING("", ring_text(r, -1));
}

static void test_ring_copies_and_bounds() {
    MsgRing r;
    ring_init(r);
    char buf[300];
    memset(buf, 'x', sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    ring_push(r, nullptr, buf);
    buf[0] = 'Z';  // caller's buffer changes after the call
    TEST_ASSERT_EQUAL_STRING("", ring_from(r, 0));
    TEST_ASSERT_EQUAL_UINT(MSG_TEXT_LEN - 1, strlen(ring_text(r, 0)));
    TEST_ASSERT_EQUAL_CHAR('x', ring_text(r, 0)[0]);
}

static void test_msg_selection_cycle() {
    TEST_ASSERT_EQUAL_INT8(-1, msg_select_next(-1, 0));
    TEST_ASSERT_EQUAL_INT8(0, msg_select_next(-1, 3));
    TEST_ASSERT_EQUAL_INT8(1, msg_select_next(0, 3));
    TEST_ASSERT_EQUAL_INT8(-1, msg_select_next(2, 3));
    TEST_ASSERT_EQUAL_INT8(-1, msg_select_next(7, 3));  // stale index after ring shrink
}

// ---------------------------------------------------------------- HIL review additions
static void test_ascii_fold() {
    char a[32] = "caf\xC3\xA9 \xF0\x9F\x98\x80!";  // "cafe" + U+1F600
    ascii_fold(a);
    TEST_ASSERT_EQUAL_STRING("caf? ?!", a);
    char b[16] = "x\xE9y";               // Latin-1 byte >= 0xC0
    ascii_fold(b);
    TEST_ASSERT_EQUAL_STRING("x?y", b);
    char c[16] = "\xA9z";                // stray continuation byte
    ascii_fold(c);
    TEST_ASSERT_EQUAL_STRING("?z", c);
    char d[16] = "a\tb\nc\x1b\x7f";      // controls -> space, '\n' kept
    ascii_fold(d);
    TEST_ASSERT_EQUAL_STRING("a b\nc  ", d);
    char e[2] = "";
    ascii_fold(e);
    TEST_ASSERT_EQUAL_STRING("", e);
    ascii_fold(nullptr);  // no crash
}

// Screen asleep; BTN1 press-down shows the SOS hold bar (wakes), release < 2 s sends
// oled_button(1,false): that release is the waking press and must do nothing else.
static void test_btn1_release_after_progress_wake_is_consumed() {
    SleepState s;
    sleep_init(s, 0);
    sleep_tick(s, 60000);
    TEST_ASSERT_FALSE(s.on);
    TEST_ASSERT_TRUE(sleep_wake_progress(s, 70000));
    TEST_ASSERT_TRUE(s.on);
    TEST_ASSERT_EQUAL_UINT8(BTN_ACT_WAKE, sleep_button(s, 1, false, 70300));
    TEST_ASSERT_EQUAL_UINT8(BTN_ACT_BTN1, sleep_button(s, 1, false, 71000));  // next press acts
    // Progress while already on does not arm the swallow.
    TEST_ASSERT_FALSE(sleep_wake_progress(s, 72000));
    TEST_ASSERT_EQUAL_UINT8(BTN_ACT_BTN1, sleep_button(s, 1, false, 72100));
    // Armed, but the press comes after the window (e.g. SOS fired, much later press).
    sleep_tick(s, 72100 + 60000);
    sleep_wake_progress(s, 140000);
    TEST_ASSERT_EQUAL_UINT8(BTN_ACT_BTN1, sleep_button(s, 1, false, 140000 + HOLD_WAKE_WINDOW_MS + 1));
    // Armed, but BTN2 comes first: page change, and the flag is cleared.
    sleep_tick(s, 300000);
    sleep_wake_progress(s, 300000);
    TEST_ASSERT_EQUAL_UINT8(BTN_ACT_NEXT_PAGE, sleep_button(s, 2, false, 300100));
    TEST_ASSERT_EQUAL_UINT8(BTN_ACT_BTN1, sleep_button(s, 1, false, 300200));
}

// Network page (AP): mirrors page_network's line budget (4 content lines after the IP).
static int ap_page(const char* ssid, const char* pass, char out[5][22]) {
    char joined[97];
    char l[3][22];
    int line = 0;
    snprintf(joined, sizeof(joined), "SSID %s", ssid);
    int n = split_lines(joined, 21, 2, &l[0][0], sizeof(l[0]));
    for (int i = 0; i < n; i++) strcpy(out[line++], l[i]);
    snprintf(joined, sizeof(joined), "Pass %s", pass);
    int m = split_lines(joined, 21, 4 - n > 3 ? 3 : 4 - n, &l[0][0], sizeof(l[0]));
    for (int i = 0; i < m; i++) strcpy(out[line++], l[i]);
    return line;
}

static void test_network_page_ap_password() {
    char out[5][22];
    // Derived default for MAC 54:43:B2:16:F1:5C: 8 uppercase hex chars, one line.
    TEST_ASSERT_EQUAL_INT(2, ap_page("Voyager", "B216F15C", out));
    TEST_ASSERT_EQUAL_STRING("SSID Voyager", out[0]);
    TEST_ASSERT_EQUAL_STRING("Pass B216F15C", out[1]);
    // Short SSID + 63-char password: 3 lines, 58 chars shown before "...".
    std::string pw(63, 'p');
    TEST_ASSERT_EQUAL_INT(4, ap_page("Voyager", pw.c_str(), out));
    TEST_ASSERT_EQUAL_STRING("...", out[3] + 18);
    // A 58-char password fits completely.
    std::string pw58(58, 'q');
    TEST_ASSERT_EQUAL_INT(4, ap_page("Voyager", pw58.c_str(), out));
    TEST_ASSERT_EQUAL_STRING("Pass qqqqqqqqqqqqqqqq", out[1]);
    TEST_ASSERT_EQUAL_UINT(21, strlen(out[3]));
    TEST_ASSERT_NULL(strchr(out[3], '.'));
    // 32-char SSID takes 2 lines; password gets the other 2 and never exceeds line 4.
    std::string ssid32(32, 's');
    TEST_ASSERT_EQUAL_INT(4, ap_page(ssid32.c_str(), pw.c_str(), out));
    TEST_ASSERT_EQUAL_STRING("ssssssssssssssss", out[1]);
    TEST_ASSERT_EQUAL_STRING("...", out[3] + 18);
    // 8-char password with a 32-char SSID still shows in full.
    TEST_ASSERT_EQUAL_INT(3, ap_page(ssid32.c_str(), "B216F15C", out));
    TEST_ASSERT_EQUAL_STRING("Pass B216F15C", out[2]);
}

// ---------------------------------------------------------------- two-colour band layout
// Header elements must stay in rows 0..OLED_HEADER_H-1 (yellow), body elements in
// rows OLED_HEADER_H..63 (blue). These are the boxes oled_display.cpp draws with.
static void assert_header(const Box& b, const char* what) {
    char msg[64];
    snprintf(msg, sizeof(msg), "%s y=%d..%d", what, b.y, box_bottom(b));
    TEST_ASSERT_TRUE_MESSAGE(box_in_header(b), msg);
    TEST_ASSERT_TRUE_MESSAGE(box_on_screen_x(b), msg);
}
static void assert_body(const Box& b, const char* what) {
    char msg[64];
    snprintf(msg, sizeof(msg), "%s y=%d..%d", what, b.y, box_bottom(b));
    TEST_ASSERT_TRUE_MESSAGE(box_in_body(b), msg);
    TEST_ASSERT_TRUE_MESSAGE(box_on_screen_x(b), msg);
}

static void test_band_constants() {
    TEST_ASSERT_EQUAL_INT(16, OLED_HEADER_H);
    TEST_ASSERT_EQUAL_INT(15, HEADER_SEP_Y);
    TEST_ASSERT_EQUAL_INT(0, BAND_TEXT_Y2);
    TEST_ASSERT_EQUAL_INT(4, BAND_TEXT_Y1);
    TEST_ASSERT_EQUAL_INT(17, CONTENT_Y);
    TEST_ASSERT_EQUAL_INT(5, CONTENT_LINES);
    TEST_ASSERT_EQUAL_INT(61, DOTS_Y);
    TEST_ASSERT_EQUAL_INT(7, HEADER_TITLE_MAX2);
    TEST_ASSERT_EQUAL_INT(14, HEADER_TITLE_MAX1);
}

static void test_header_elements_in_band() {
    assert_header(header_sep_box(), "separator");
    assert_header(header_title_box(2), "title size 2");
    assert_header(header_title_box(1), "title size 1");
    assert_header(icon_wifi_box(), "wifi icon");
    assert_header(icon_lora_box(), "lora icon");
    assert_header(icon_bars_box(), "bars/AP");
    // Ink (caps at size 2, icons) sits above the separator row, not on it.
    TEST_ASSERT_TRUE(BAND_TEXT_Y2 + BAND_INK_H2 - 1 < HEADER_SEP_Y);
    TEST_ASSERT_TRUE(BAND_TEXT_Y1 + CHAR_H - 1 < HEADER_SEP_Y);   // size-1 cell incl. descender
    TEST_ASSERT_TRUE(box_bottom(icon_wifi_box()) < HEADER_SEP_Y);
    // Title, icons and the AP/bars slot do not collide horizontally.
    TEST_ASSERT_FALSE(boxes_overlap(header_title_box(2), icon_wifi_box()));
    TEST_ASSERT_FALSE(boxes_overlap(header_title_box(1), icon_wifi_box()));
    TEST_ASSERT_FALSE(boxes_overlap(icon_wifi_box(), icon_lora_box()));
    TEST_ASSERT_FALSE(boxes_overlap(icon_lora_box(), icon_bars_box()));
    TEST_ASSERT_TRUE(text_width("AP") <= ICON_BARS_W);
    TEST_ASSERT_TRUE(ICON_BARS_X + 3 * 3 + 2 <= SCREEN_W);       // 4th bar's right column
    // The wifi arcs (r = 6 around the icon centre) stay inside its box.
    TEST_ASSERT_TRUE(ICON_WIFI_X + ICON_WIFI_W / 2 - 6 >= ICON_WIFI_X);
    TEST_ASSERT_TRUE(ICON_WIFI_X + ICON_WIFI_W / 2 + 6 <= box_right(icon_wifi_box()));
    TEST_ASSERT_TRUE(ICON_BASE_Y - 6 >= ICON_TOP_Y);
}

static void test_body_elements_below_band() {
    for (int i = 0; i < CONTENT_LINES; i++) {
        assert_body(content_line_box(i), "content line");
        TEST_ASSERT_TRUE(box_bottom(content_line_box(i)) < DOTS_Y);  // never touches the dots
    }
    // Messages detail view: inverted sender bar from CONTENT_Y-1.
    Box sender = {0, CONTENT_Y - 1, SCREEN_W, LINE_STEP};
    assert_body(sender, "sender bar");
    assert_body(dots_box(), "page dots");
    for (int i = 0; i < ALERT_LINES; i++) {
        assert_body(alert_line_box(i), "alert line");
        TEST_ASSERT_TRUE(box_bottom(alert_line_box(i)) < box_bottom(alert_frame_box()));  // inside frame
    }
    assert_body(alert_frame_box(), "alert frame");
    assert_header(alert_band_box(), "alert band");
    assert_body(progress_bar_box(), "progress bar");
    assert_body(progress_pct_box(), "progress pct");
    TEST_ASSERT_FALSE(boxes_overlap(progress_bar_box(), progress_pct_box()));
    assert_body(msgov_from_box(2), "msg from size 2");
    assert_body(msgov_from_box(1), "msg from size 1");
    for (int i = 0; i < MSGOV_TEXT_LINES; i++) {
        assert_body(msgov_text_box(i), "msg text");
        TEST_ASSERT_FALSE(boxes_overlap(msgov_from_box(2), msgov_text_box(i)));
    }
    for (int f = 0; f < SPLASH_FRAMES; f++) assert_body(splash_ring_box(f), "splash ring");
    assert_body(splash_tag_box(), "splash tag");
}

static void test_band_title_size_and_case() {
    TEST_ASSERT_EQUAL_UINT8(2, band_text_size("Voyager", HEADER_TITLE_PX));
    TEST_ASSERT_EQUAL_UINT8(2, band_text_size("Network", HEADER_TITLE_PX));
    TEST_ASSERT_EQUAL_UINT8(2, band_text_size("Msg 5/5", HEADER_TITLE_PX));
    TEST_ASSERT_EQUAL_UINT8(2, band_text_size("Inbox", HEADER_TITLE_PX));
    TEST_ASSERT_EQUAL_UINT8(1, band_text_size("Messages", HEADER_TITLE_PX));  // why it is "Inbox"
    TEST_ASSERT_EQUAL_UINT8(2, band_text_size("TX failed", SCREEN_W - 2 * OVERLAY_MARGIN));
    TEST_ASSERT_EQUAL_UINT8(1, band_text_size("Storage error", SCREEN_W - 2 * OVERLAY_MARGIN));
    TEST_ASSERT_EQUAL_UINT8(1, band_text_size("Hold for SOS", SCREEN_W));
    TEST_ASSERT_EQUAL_INT(BAND_TEXT_Y2, band_text_y(2));
    TEST_ASSERT_EQUAL_INT(BAND_TEXT_Y1, band_text_y(1));
    char t[16] = "Msg 1/5 gpqy,";
    ascii_upper(t);
    TEST_ASSERT_EQUAL_STRING("MSG 1/5 GPQY,", t);
    ascii_upper(nullptr);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_safe_copy_null_and_bounds);
    RUN_TEST(test_chars_fit_and_width);
    RUN_TEST(test_truncate_ellipsis);
    RUN_TEST(test_center_and_clamp_x);
    RUN_TEST(test_wrap_basic_words);
    RUN_TEST(test_wrap_empty_and_null);
    RUN_TEST(test_wrap_long_word_hard_split);
    RUN_TEST(test_wrap_overflow_gets_ellipsis);
    RUN_TEST(test_wrap_newlines_and_tabs);
    RUN_TEST(test_wrap_clamps_to_stride);
    RUN_TEST(test_split_lines);
    RUN_TEST(test_progress_width);
    RUN_TEST(test_rssi_bars);
    RUN_TEST(test_uptime_format);
    RUN_TEST(test_page_cycling);
    RUN_TEST(test_sleep_after_60s_idle);
    RUN_TEST(test_wake_press_is_consumed);
    RUN_TEST(test_long_press_toggles);
    RUN_TEST(test_activity_restarts_timer_and_wake);
    RUN_TEST(test_sleep_millis_wrap);
    RUN_TEST(test_ring_newest_first_and_capacity);
    RUN_TEST(test_ring_copies_and_bounds);
    RUN_TEST(test_msg_selection_cycle);
    RUN_TEST(test_ascii_fold);
    RUN_TEST(test_btn1_release_after_progress_wake_is_consumed);
    RUN_TEST(test_network_page_ap_password);
    RUN_TEST(test_band_constants);
    RUN_TEST(test_header_elements_in_band);
    RUN_TEST(test_body_elements_below_band);
    RUN_TEST(test_band_title_size_and_case);
    return UNITY_END();
}
