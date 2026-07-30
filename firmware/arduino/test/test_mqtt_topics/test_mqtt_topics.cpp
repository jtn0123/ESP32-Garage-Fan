// Unit tests for MQTT topic generation and payload formatting
// Tests the topic building logic extracted from mqtt_client.cpp

#include <unity.h>
#include <cstdarg>  // va_start/va_end used by safe_snprintf_rt below
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cmath>

// Safe string formatting (mirrors firmware implementation)
static int safe_snprintf_rt(char* buf, size_t size, const char* fmt, ...) {
    if (!buf || size == 0) return 0;
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, size, fmt, args);
    va_end(args);
    if (ret < 0) ret = 0;
    if ((size_t)ret >= size) ret = (int)size - 1;
    buf[size - 1] = '\0';
    return ret;
}

// Topic building logic (extracted from mqtt_client.cpp for testing)
static char g_mqtt_client_id[40] = "";

static void build_topic_buf(char* out, size_t out_size, const char* suffix) {
    if (out_size == 0) return;
    if (g_mqtt_client_id[0] == '\0') {
        safe_snprintf_rt(out, out_size, "espsensor/unknown/%s", suffix ? suffix : "");
        return;
    }
    safe_snprintf_rt(out, out_size, "espsensor/%s/%s", g_mqtt_client_id, suffix ? suffix : "");
}

static void mqtt_set_client_id(const char* client_id) {
    if (client_id) {
        snprintf(g_mqtt_client_id, sizeof(g_mqtt_client_id), "%s", client_id);
    }
}

// Payload formatting functions (mirrors firmware implementation)
static void format_temperature_payload(char* buf, size_t size, float tempC) {
    if (std::isfinite(tempC)) {
        snprintf(buf, size, "%.1f", tempC);
    } else {
        buf[0] = '\0';
    }
}

static void format_humidity_payload(char* buf, size_t size, float rhPct) {
    if (std::isfinite(rhPct)) {
        snprintf(buf, size, "%.1f", rhPct);
    } else {
        buf[0] = '\0';
    }
}

static void format_battery_voltage_payload(char* buf, size_t size, float voltage) {
    if (std::isfinite(voltage)) {
        snprintf(buf, size, "%.2f", voltage);
    } else {
        buf[0] = '\0';
    }
}

static void format_battery_percent_payload(char* buf, size_t size, int percent) {
    if (percent >= 0) {
        snprintf(buf, size, "%d", percent);
    } else {
        buf[0] = '\0';
    }
}

static void format_rssi_payload(char* buf, size_t size, int rssiDbm) {
    snprintf(buf, size, "%d", rssiDbm);
}

void setUp() {
    g_mqtt_client_id[0] = '\0';
}

void tearDown() {
    // Nothing to clean up
}

// === Topic Building Tests ===

void test_topic_with_valid_client_id() {
    mqtt_set_client_id("office_sensor");
    char topic[96];

    build_topic_buf(topic, sizeof(topic), "inside/temperature");
    TEST_ASSERT_EQUAL_STRING("espsensor/office_sensor/inside/temperature", topic);

    build_topic_buf(topic, sizeof(topic), "inside/humidity");
    TEST_ASSERT_EQUAL_STRING("espsensor/office_sensor/inside/humidity", topic);

    build_topic_buf(topic, sizeof(topic), "battery/voltage");
    TEST_ASSERT_EQUAL_STRING("espsensor/office_sensor/battery/voltage", topic);
}

void test_topic_without_client_id() {
    char topic[96];

    build_topic_buf(topic, sizeof(topic), "inside/temperature");
    TEST_ASSERT_EQUAL_STRING("espsensor/unknown/inside/temperature", topic);
}

void test_topic_with_empty_suffix() {
    mqtt_set_client_id("test_device");
    char topic[96];

    build_topic_buf(topic, sizeof(topic), "");
    TEST_ASSERT_EQUAL_STRING("espsensor/test_device/", topic);
}

void test_topic_with_null_suffix() {
    mqtt_set_client_id("test_device");
    char topic[96];

    build_topic_buf(topic, sizeof(topic), nullptr);
    TEST_ASSERT_EQUAL_STRING("espsensor/test_device/", topic);
}

void test_topic_buffer_truncation() {
    mqtt_set_client_id("very_long_device_name_that_exceeds_normal_length");
    char small_buf[30];

    build_topic_buf(small_buf, sizeof(small_buf), "inside/temperature");
    // Should be truncated but null-terminated
    TEST_ASSERT_EQUAL('\0', small_buf[29]);
    TEST_ASSERT_TRUE(strlen(small_buf) < 30);
}

void test_all_standard_topic_suffixes() {
    mqtt_set_client_id("room1");
    char topic[96];
    const char* expected_prefix = "espsensor/room1/";

    // All topic suffixes used by firmware
    const char* suffixes[] = {
        "inside/temperature",
        "inside/humidity",
        "inside/pressure",
        "battery/voltage",
        "battery/percent",
        "wifi/rssi",
        "availability",
        "status",
        "debug/json",
        "debug/last_crash",
        "debug/probe",
        "debug/boot_reason",
        "debug/boot_count",
        "debug/crash_count",
        "debug/uptime",
        "debug/wake_count",
        "debug/memory",
        "diagnostic_mode",
        "debug/publish_latency_ms",
        "cmd/+"
    };

    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        build_topic_buf(topic, sizeof(topic), suffixes[i]);

        // Verify prefix
        TEST_ASSERT_TRUE_MESSAGE(strncmp(topic, expected_prefix, strlen(expected_prefix)) == 0,
                                  suffixes[i]);

        // Verify suffix is appended
        const char* actual_suffix = topic + strlen(expected_prefix);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(suffixes[i], actual_suffix, suffixes[i]);
    }
}

// === Payload Formatting Tests ===

void test_temperature_payload_formatting() {
    char buf[32];

    format_temperature_payload(buf, sizeof(buf), 22.5f);
    TEST_ASSERT_EQUAL_STRING("22.5", buf);

    format_temperature_payload(buf, sizeof(buf), 0.0f);
    TEST_ASSERT_EQUAL_STRING("0.0", buf);

    format_temperature_payload(buf, sizeof(buf), -10.3f);
    TEST_ASSERT_EQUAL_STRING("-10.3", buf);

    // Precision: should round to 1 decimal
    format_temperature_payload(buf, sizeof(buf), 22.56f);
    TEST_ASSERT_EQUAL_STRING("22.6", buf);

    format_temperature_payload(buf, sizeof(buf), 22.54f);
    TEST_ASSERT_EQUAL_STRING("22.5", buf);
}

void test_temperature_payload_nan() {
    char buf[32] = "initial";

    format_temperature_payload(buf, sizeof(buf), NAN);
    TEST_ASSERT_EQUAL_STRING("", buf);

    format_temperature_payload(buf, sizeof(buf), INFINITY);
    TEST_ASSERT_EQUAL_STRING("", buf);

    format_temperature_payload(buf, sizeof(buf), -INFINITY);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_humidity_payload_formatting() {
    char buf[32];

    format_humidity_payload(buf, sizeof(buf), 45.0f);
    TEST_ASSERT_EQUAL_STRING("45.0", buf);

    format_humidity_payload(buf, sizeof(buf), 100.0f);
    TEST_ASSERT_EQUAL_STRING("100.0", buf);

    format_humidity_payload(buf, sizeof(buf), 0.0f);
    TEST_ASSERT_EQUAL_STRING("0.0", buf);

    format_humidity_payload(buf, sizeof(buf), 67.89f);
    TEST_ASSERT_EQUAL_STRING("67.9", buf);
}

void test_battery_voltage_formatting() {
    char buf[32];

    format_battery_voltage_payload(buf, sizeof(buf), 4.20f);
    TEST_ASSERT_EQUAL_STRING("4.20", buf);

    format_battery_voltage_payload(buf, sizeof(buf), 3.70f);
    TEST_ASSERT_EQUAL_STRING("3.70", buf);

    format_battery_voltage_payload(buf, sizeof(buf), 3.123f);
    TEST_ASSERT_EQUAL_STRING("3.12", buf);

    format_battery_voltage_payload(buf, sizeof(buf), 3.126f);
    TEST_ASSERT_EQUAL_STRING("3.13", buf);
}

void test_battery_voltage_invalid() {
    char buf[32] = "initial";

    format_battery_voltage_payload(buf, sizeof(buf), NAN);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_battery_percent_formatting() {
    char buf[32];

    format_battery_percent_payload(buf, sizeof(buf), 100);
    TEST_ASSERT_EQUAL_STRING("100", buf);

    format_battery_percent_payload(buf, sizeof(buf), 50);
    TEST_ASSERT_EQUAL_STRING("50", buf);

    format_battery_percent_payload(buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL_STRING("0", buf);
}

void test_battery_percent_invalid() {
    char buf[32] = "initial";

    format_battery_percent_payload(buf, sizeof(buf), -1);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_rssi_formatting() {
    char buf[32];

    format_rssi_payload(buf, sizeof(buf), -50);
    TEST_ASSERT_EQUAL_STRING("-50", buf);

    format_rssi_payload(buf, sizeof(buf), -100);
    TEST_ASSERT_EQUAL_STRING("-100", buf);

    format_rssi_payload(buf, sizeof(buf), 0);
    TEST_ASSERT_EQUAL_STRING("0", buf);
}

// === Discovery Topic Tests ===

void test_discovery_topic_format() {
    // Home Assistant discovery topic format
    char discovery_topic[96];
    const char* device_id = "office_sensor";

    snprintf(discovery_topic, sizeof(discovery_topic),
             "homeassistant/sensor/%s_%s/config", device_id, "temperature");
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/office_sensor_temperature/config",
                              discovery_topic);

    snprintf(discovery_topic, sizeof(discovery_topic),
             "homeassistant/sensor/%s_%s/config", device_id, "humidity");
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/office_sensor_humidity/config",
                              discovery_topic);
}

// === Client ID Validation Tests ===

void test_client_id_with_special_chars() {
    // Client IDs with common special characters
    mqtt_set_client_id("room-1");
    char topic[96];
    build_topic_buf(topic, sizeof(topic), "status");
    TEST_ASSERT_EQUAL_STRING("espsensor/room-1/status", topic);

    mqtt_set_client_id("room_1");
    build_topic_buf(topic, sizeof(topic), "status");
    TEST_ASSERT_EQUAL_STRING("espsensor/room_1/status", topic);
}

void test_client_id_max_length() {
    // Test with maximum length client ID
    char long_id[40];
    memset(long_id, 'x', 39);
    long_id[39] = '\0';

    mqtt_set_client_id(long_id);
    TEST_ASSERT_EQUAL_STRING(long_id, g_mqtt_client_id);

    char topic[128];
    build_topic_buf(topic, sizeof(topic), "status");
    TEST_ASSERT_TRUE(strlen(topic) > 0);
}

// === Memory Diagnostics Payload Test ===

void test_memory_diagnostics_json_format() {
    char payload[128];
    uint32_t free_heap = 150000;
    uint32_t min_heap = 120000;
    uint32_t largest_block = 80000;
    float fragmentation_pct = 15.5f;

    snprintf(payload, sizeof(payload),
             "{\"free\":%u,\"min\":%u,\"largest\":%u,\"frag\":%.1f}",
             free_heap, min_heap, largest_block, fragmentation_pct);

    TEST_ASSERT_EQUAL_STRING(
        "{\"free\":150000,\"min\":120000,\"largest\":80000,\"frag\":15.5}",
        payload);
}

// === Status Payload Test ===

void test_status_json_format() {
    char payload[256];
    const char* mode = "production";
    uint32_t sleep_interval = 600;
    uint32_t dev_timeout_sec = 0;
    uint32_t uptime_sec = 3600;
    int battery_pct = 85;
    uint32_t heap_free = 150000;
    const char* fw_version = "1.0.0";
    const char* room_name = "Office";

    snprintf(payload, sizeof(payload),
        "{\"mode\":\"%s\",\"sleep_interval_sec\":%lu,\"dev_mode_timeout_sec\":%lu,"
        "\"uptime_sec\":%lu,\"battery_pct\":%d,\"heap_free\":%lu,\"fw_version\":\"%s\",\"room\":\"%s\"}",
        mode,
        (unsigned long)sleep_interval,
        (unsigned long)dev_timeout_sec,
        (unsigned long)uptime_sec,
        battery_pct,
        (unsigned long)heap_free,
        fw_version,
        room_name);

    // Verify it's valid JSON-like structure
    TEST_ASSERT_TRUE(payload[0] == '{');
    TEST_ASSERT_TRUE(payload[strlen(payload) - 1] == '}');
    TEST_ASSERT_NOT_NULL(strstr(payload, "\"mode\":\"production\""));
    TEST_ASSERT_NOT_NULL(strstr(payload, "\"sleep_interval_sec\":600"));
    TEST_ASSERT_NOT_NULL(strstr(payload, "\"battery_pct\":85"));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Topic building tests
    RUN_TEST(test_topic_with_valid_client_id);
    RUN_TEST(test_topic_without_client_id);
    RUN_TEST(test_topic_with_empty_suffix);
    RUN_TEST(test_topic_with_null_suffix);
    RUN_TEST(test_topic_buffer_truncation);
    RUN_TEST(test_all_standard_topic_suffixes);

    // Payload formatting tests
    RUN_TEST(test_temperature_payload_formatting);
    RUN_TEST(test_temperature_payload_nan);
    RUN_TEST(test_humidity_payload_formatting);
    RUN_TEST(test_battery_voltage_formatting);
    RUN_TEST(test_battery_voltage_invalid);
    RUN_TEST(test_battery_percent_formatting);
    RUN_TEST(test_battery_percent_invalid);
    RUN_TEST(test_rssi_formatting);

    // Discovery tests
    RUN_TEST(test_discovery_topic_format);

    // Client ID tests
    RUN_TEST(test_client_id_with_special_chars);
    RUN_TEST(test_client_id_max_length);

    // JSON payload tests
    RUN_TEST(test_memory_diagnostics_json_format);
    RUN_TEST(test_status_json_format);

    return UNITY_END();
}
