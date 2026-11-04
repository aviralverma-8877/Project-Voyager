// L2 tests for the OLED layout/state logic (src/oled_layout.h). Owner: oled-display-engineer.
// Run: ~/.platformio/penv/bin/pio test -e native -f test_oled
#include <unity.h>
#include <string.h>
#include <string>
#include "oled_layout.h"

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
    return UNITY_END();
}
