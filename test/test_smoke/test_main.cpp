// Smoke test for [env:native]: proves Unity, gnu++17, UNIT_TEST and ArduinoJson
// all work on the host. Component tests live in test/test_<component>/.
#include <unity.h>
#include <ArduinoJson.h>
#include <string>

#ifndef UNIT_TEST
#error "UNIT_TEST must be defined by env:native"
#endif

void setUp() {}
void tearDown() {}

static void test_arithmetic() { TEST_ASSERT_EQUAL_INT(4, 2 + 2); }

static void test_cpp17() {
    if constexpr (__cplusplus >= 201703L) {
        TEST_PASS();
    } else {
        TEST_FAIL_MESSAGE("expected C++17");
    }
}

static void test_arduinojson_roundtrip() {
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, "{\"pack_type\":\"msg\",\"data\":\"hi\"}"));
    TEST_ASSERT_EQUAL_STRING("msg", doc["pack_type"].as<const char*>());
    std::string out;
    serializeJson(doc, out);
    TEST_ASSERT_EQUAL_STRING("{\"pack_type\":\"msg\",\"data\":\"hi\"}", out.c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_arithmetic);
    RUN_TEST(test_cpp17);
    RUN_TEST(test_arduinojson_roundtrip);
    return UNITY_END();
}
