// L2 native tests for src/sensors/sensor_config.h: config validation, pin
// rules and the over-the-air sensor payload.
#include <unity.h>
#include <string>
#include "sensors/sensor_config.h"

using namespace sensor_cfg;

// esp_lora_v1 pin map (platformio.ini).
static const int8_t V1[] = {2, 15, 23, 19, 5, 18, 14, 32, 36, 4, 22, 21};
static const size_t V1_N = sizeof(V1) / sizeof(V1[0]);
// esp_lora_v2 pin map.
static const int8_t V2[] = {26, 33, 32, 27, 19, 18, 5, 23, 39, 36, 2, 22, 21};
static const size_t V2_N = sizeof(V2) / sizeof(V2[0]);

void setUp() {}
void tearDown() {}

static bool parse(const char* text, Config& c, const char** err = nullptr) {
    return config_from_text(text, c, V1, V1_N, err);
}

static void test_type_names_roundtrip() {
    for (uint8_t t = T_DHT11; t <= T_DIGITAL; t++)
        TEST_ASSERT_EQUAL_UINT8(t, type_from_name(type_name(t)));
    TEST_ASSERT_EQUAL_UINT8(T_NONE, type_from_name("bme280"));
    TEST_ASSERT_EQUAL_UINT8(T_NONE, type_from_name(nullptr));
    TEST_ASSERT_EQUAL_UINT8(2, value_count(T_DHT22));
    TEST_ASSERT_EQUAL_UINT8(1, value_count(T_HCSR04));
    TEST_ASSERT_EQUAL_UINT8(0, value_count(T_NONE));
    // Over-the-air ids are fixed.
    TEST_ASSERT_EQUAL_UINT8(1, T_DHT11);
    TEST_ASSERT_EQUAL_UINT8(6, T_DIGITAL);
}

static void test_pin_rules_per_board() {
    // Board pins are never offered.
    TEST_ASSERT_FALSE(pin_ok_io(4, V1, V1_N));     // v1 LED
    TEST_ASSERT_TRUE(pin_ok_io(4, V2, V2_N));
    TEST_ASSERT_TRUE(pin_ok_io(26, V1, V1_N));
    TEST_ASSERT_FALSE(pin_ok_io(26, V2, V2_N));    // v2 DIO0
    // GPIO 15: free on v2, the radio's DIO1 on v1; digital only (ADC2).
    TEST_ASSERT_TRUE(pin_ok_io(15, V2, V2_N));
    TEST_ASSERT_FALSE(pin_ok_io(15, V1, V1_N));
    TEST_ASSERT_FALSE(pin_ok_adc(15, V2, V2_N));
    // Flash, UART and the other strapping pins are never candidates.
    const int bad[] = {0, 1, 2, 3, 5, 6, 11, 12, 40, -1};
    for (int p : bad) {
        TEST_ASSERT_FALSE(pin_ok_io(p, nullptr, 0));
        TEST_ASSERT_FALSE(pin_ok_input(p, nullptr, 0));
    }
    // Input-only pins can read but not drive.
    TEST_ASSERT_FALSE(pin_ok_io(34, V1, V1_N));
    TEST_ASSERT_TRUE(pin_ok_input(34, V1, V1_N));
    TEST_ASSERT_FALSE(pin_ok_input(36, V1, V1_N)); // BTN2
    // Analog: ADC1 only (ADC2 is unusable with WiFi on).
    TEST_ASSERT_TRUE(pin_ok_adc(33, V1, V1_N));
    TEST_ASSERT_FALSE(pin_ok_adc(33, V2, V2_N));   // v2 DIO1
    TEST_ASSERT_TRUE(pin_ok_adc(35, V2, V2_N));
    TEST_ASSERT_FALSE(pin_ok_adc(25, V1, V1_N));   // ADC2
    TEST_ASSERT_FALSE(pin_ok_adc(13, V1, V1_N));
}

static void test_name_rules() {
    TEST_ASSERT_TRUE(name_valid("room"));
    TEST_ASSERT_TRUE(name_valid("Tank_1-a"));
    TEST_ASSERT_TRUE(name_valid("abcdefghijkl"));     // 12
    TEST_ASSERT_FALSE(name_valid("abcdefghijklm"));   // 13
    TEST_ASSERT_FALSE(name_valid(""));
    TEST_ASSERT_FALSE(name_valid(nullptr));
    TEST_ASSERT_FALSE(name_valid("a b"));
    TEST_ASSERT_FALSE(name_valid("a\"b"));
    TEST_ASSERT_FALSE(name_valid("<b>"));
}

static void test_config_valid_and_roundtrip() {
    Config c;
    const char* err = nullptr;
    const char* text =
        "{\"enabled\":true,\"interval\":60,\"ack\":true,\"sensors\":["
        "{\"type\":\"dht22\",\"name\":\"room\",\"pin\":13,\"pin2\":-1},"
        "{\"type\":\"hcsr04\",\"name\":\"tank\",\"pin\":16,\"pin2\":34},"
        "{\"type\":\"analog\",\"name\":\"light\",\"pin\":35},"
        "{\"type\":\"digital\",\"name\":\"door\",\"pin\":39}]}";
    TEST_ASSERT_TRUE_MESSAGE(parse(text, c, &err), err ? err : "");
    TEST_ASSERT_TRUE(c.enabled);
    TEST_ASSERT_TRUE(c.ack);
    TEST_ASSERT_EQUAL(60, c.interval);
    TEST_ASSERT_EQUAL_UINT8(4, c.count);
    TEST_ASSERT_EQUAL_UINT8(T_HCSR04, c.sensors[1].type);
    TEST_ASSERT_EQUAL_INT8(34, c.sensors[1].pin2);
    TEST_ASSERT_EQUAL_INT8(-1, c.sensors[0].pin2);
    TEST_ASSERT_EQUAL_STRING("light", c.sensors[2].name);

    Config again;
    std::string out = config_to_text(c);
    TEST_ASSERT_TRUE(out.size() <= CONFIG_MAX_BYTES);
    TEST_ASSERT_TRUE(parse(out.c_str(), again));
    TEST_ASSERT_EQUAL_STRING(out.c_str(), config_to_text(again).c_str());
}

static void test_config_disabled_empty_ok() {
    Config c;
    TEST_ASSERT_TRUE(parse("{\"enabled\":false,\"interval\":30,\"ack\":false,\"sensors\":[]}", c));
    TEST_ASSERT_FALSE(c.enabled);
    TEST_ASSERT_EQUAL_UINT8(0, c.count);
}

static void test_config_rejects() {
    const char* bad[] = {
        "",
        "[]",
        "{nope",
        "{\"interval\":30,\"ack\":false,\"sensors\":[]}",                                   // enabled missing
        "{\"enabled\":\"true\",\"interval\":30,\"ack\":false,\"sensors\":[]}",              // wrong type
        "{\"enabled\":false,\"interval\":30,\"sensors\":[]}",                               // ack missing
        "{\"enabled\":false,\"interval\":1,\"ack\":false,\"sensors\":[]}",                  // too fast
        "{\"enabled\":false,\"interval\":86401,\"ack\":false,\"sensors\":[]}",
        "{\"enabled\":false,\"interval\":2.5,\"ack\":false,\"sensors\":[]}",
        "{\"enabled\":false,\"interval\":30,\"ack\":false}",                                // no list
        "{\"enabled\":true,\"interval\":30,\"ack\":false,\"sensors\":[]}",                  // on, nothing to read
        "{\"enabled\":false,\"interval\":30,\"ack\":false,\"sensors\":[{\"type\":\"bme280\",\"name\":\"a\",\"pin\":13}]}",
        "{\"enabled\":false,\"interval\":30,\"ack\":false,\"sensors\":[{\"type\":\"dht11\",\"name\":\"a b\",\"pin\":13}]}",
        "{\"enabled\":false,\"interval\":30,\"ack\":false,\"sensors\":[{\"type\":\"dht11\",\"name\":\"a\"}]}",
        "{\"enabled\":false,\"interval\":30,\"ack\":false,\"sensors\":[{\"type\":\"dht11\",\"name\":\"a\",\"pin\":4}]}",   // v1 LED
        "{\"enabled\":false,\"interval\":30,\"ack\":false,\"sensors\":[{\"type\":\"dht11\",\"name\":\"a\",\"pin\":34}]}",  // input only
        "{\"enabled\":false,\"interval\":30,\"ack\":false,\"sensors\":[{\"type\":\"analog\",\"name\":\"a\",\"pin\":25}]}", // ADC2
        "{\"enabled\":false,\"interval\":30,\"ack\":false,\"sensors\":[{\"type\":\"hcsr04\",\"name\":\"a\",\"pin\":13}]}", // no echo
        "{\"enabled\":false,\"interval\":30,\"ack\":false,\"sensors\":[{\"type\":\"hcsr04\",\"name\":\"a\",\"pin\":13,\"pin2\":13}]}",
        // duplicate name / duplicate pin
        "{\"enabled\":false,\"interval\":30,\"ack\":false,\"sensors\":[{\"type\":\"dht11\",\"name\":\"a\",\"pin\":13},{\"type\":\"dht22\",\"name\":\"a\",\"pin\":16}]}",
        "{\"enabled\":false,\"interval\":30,\"ack\":false,\"sensors\":[{\"type\":\"dht11\",\"name\":\"a\",\"pin\":13},{\"type\":\"dht22\",\"name\":\"b\",\"pin\":13}]}",
        // five sensors
        "{\"enabled\":false,\"interval\":30,\"ack\":false,\"sensors\":[{\"type\":\"digital\",\"name\":\"a\",\"pin\":13},{\"type\":\"digital\",\"name\":\"b\",\"pin\":16},{\"type\":\"digital\",\"name\":\"c\",\"pin\":17},{\"type\":\"digital\",\"name\":\"d\",\"pin\":25},{\"type\":\"digital\",\"name\":\"e\",\"pin\":26}]}",
    };
    for (const char* t : bad) {
        Config c;
        c.interval = 777;   // must stay untouched on failure
        const char* err = nullptr;
        TEST_ASSERT_FALSE_MESSAGE(parse(t, c, &err), t);
        TEST_ASSERT_NOT_NULL(err);
        TEST_ASSERT_EQUAL(777, c.interval);
    }
    Config c;
    TEST_ASSERT_FALSE(config_from_text(nullptr, c, V1, V1_N));
    std::string huge(CONFIG_MAX_BYTES + 1, ' ');
    TEST_ASSERT_FALSE(config_from_text(huge.c_str(), c, V1, V1_N));
}

static Reading mk(const char* name, uint8_t type, bool ok, float a, float b = 0) {
    Reading r;
    strcpy(r.name, name);
    r.type = type;
    r.ok = ok;
    r.v[0] = a;
    r.v[1] = b;
    return r;
}

static void test_reading_json_format() {
    TEST_ASSERT_EQUAL_STRING("[\"room\",2,23.5,40.1]", reading_json(mk("room", T_DHT22, true, 23.5f, 40.12f)).c_str());
    TEST_ASSERT_EQUAL_STRING("[\"out\",3,-5.2]", reading_json(mk("out", T_DS18B20, true, -5.19f)).c_str());
    TEST_ASSERT_EQUAL_STRING("[\"light\",5,2048]", reading_json(mk("light", T_ANALOG, true, 2048.0f)).c_str());
    TEST_ASSERT_EQUAL_STRING("[\"door\",6,1]", reading_json(mk("door", T_DIGITAL, true, 1.0f)).c_str());
    // Failed read: null per value.
    TEST_ASSERT_EQUAL_STRING("[\"room\",1,null,null]", reading_json(mk("room", T_DHT11, false, 0)).c_str());
    TEST_ASSERT_EQUAL_STRING("[\"tank\",4,null]", reading_json(mk("tank", T_HCSR04, false, 0)).c_str());
}

static void test_inner_json_is_valid_json() {
    Reading r[2] = {mk("room", T_DHT22, true, 23.5f, 40.1f), mk("tank", T_HCSR04, false, 0)};
    std::string inner = inner_json(r, 2);
    JsonDocument doc;
    TEST_ASSERT_FALSE((bool)deserializeJson(doc, inner));
    TEST_ASSERT_EQUAL_STRING("sensor", doc["pack_type"] | "");
    TEST_ASSERT_EQUAL(2, doc["data"].size());
    TEST_ASSERT_EQUAL_STRING("room", doc["data"][0][0] | "");
    TEST_ASSERT_EQUAL(2, doc["data"][0][1].as<int>());
    TEST_ASSERT_TRUE(doc["data"][1][2].isNull());
}

static void test_pack_fits_everything_normally() {
    Reading r[4] = {mk("room", T_DHT22, true, 23.5f, 40.1f), mk("tank", T_HCSR04, true, 120.4f),
                    mk("light", T_ANALOG, true, 2048), mk("door", T_DIGITAL, true, 1)};
    std::string inner;
    size_t taken = pack(r, 4, 0, "node1", "AA:BB:CC:DD:EE:FF", 251, inner);
    TEST_ASSERT_EQUAL_size_t(4, taken);
    TEST_ASSERT_TRUE(wrapped_len("node1", "AA:BB:CC:DD:EE:FF", inner) <= 251);
}

static void test_pack_splits_when_too_long() {
    // Worst case: 32-char username full of characters JSON must escape, long names.
    std::string user(32, '"');
    Reading r[4] = {mk("abcdefghijkl", T_DHT22, true, -39.9f, 100.0f), mk("bbcdefghijkl", T_DHT22, true, -39.9f, 100.0f),
                    mk("cbcdefghijkl", T_DHT22, true, -39.9f, 100.0f), mk("dbcdefghijkl", T_DHT22, true, -39.9f, 100.0f)};
    size_t start = 0, packets = 0;
    while (start < 4) {
        std::string inner;
        size_t taken = pack(r, 4, start, user.c_str(), "AA:BB:CC:DD:EE:FF", 251, inner);
        TEST_ASSERT_TRUE_MESSAGE(taken >= 1, "a single reading must always fit");
        TEST_ASSERT_TRUE(wrapped_len(user.c_str(), "AA:BB:CC:DD:EE:FF", inner) <= 251);
        start += taken;
        packets++;
    }
    TEST_ASSERT_TRUE(packets > 1);
    TEST_ASSERT_EQUAL_size_t(4, start);
}

static void test_pack_nothing_fits() {
    Reading r[1] = {mk("room", T_DHT22, true, 1, 2)};
    std::string inner = "stale";
    TEST_ASSERT_EQUAL_size_t(0, pack(r, 1, 0, "n", "m", 20, inner));
    TEST_ASSERT_EQUAL_STRING("", inner.c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_type_names_roundtrip);
    RUN_TEST(test_pin_rules_per_board);
    RUN_TEST(test_name_rules);
    RUN_TEST(test_config_valid_and_roundtrip);
    RUN_TEST(test_config_disabled_empty_ok);
    RUN_TEST(test_config_rejects);
    RUN_TEST(test_reading_json_format);
    RUN_TEST(test_inner_json_is_valid_json);
    RUN_TEST(test_pack_fits_everything_normally);
    RUN_TEST(test_pack_splits_when_too_long);
    RUN_TEST(test_pack_nothing_fits);
    return UNITY_END();
}
