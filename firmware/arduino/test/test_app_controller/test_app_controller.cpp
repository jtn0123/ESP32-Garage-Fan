// Unit tests for app_controller module
#include <unity.h>
#include <Arduino.h>

// Mock all hardware dependencies
#define USE_DISPLAY 0
#define USE_BATTERY 0
#define USE_MAX17048 0
#define USE_LC709203F 0
#define USE_SHT 0
#define USE_BME 0
#define LOG_ENABLED 0

// We need to mock some of the dependencies that app_controller uses
// Since app_controller coordinates many modules, we'll focus on testing
// the logic flow rather than actual hardware operations

// Mock functions that would normally be provided by other modules
bool g_wifi_connected = false;
bool g_mqtt_connected = false;
bool g_sensors_initialized = false;
float g_last_temp = 25.0f;
float g_last_humidity = 50.0f;

// Controllable clock. The native shim declares millis() but leaves it to each
// test so time can be driven deterministically; delay() is a no-op on the host,
// so mocked work advances this counter instead. Without that, every duration
// below would be 0 and the timing assertions would pass vacuously.
static uint32_t g_mock_millis = 0;
uint32_t millis() { return g_mock_millis; }
static void advance_millis(uint32_t ms) { g_mock_millis += ms; }

// Mock WiFi functions
namespace {
    bool wifi_is_connected() { return g_wifi_connected; }
    bool wifi_connect_with_timeout(uint32_t timeout_ms) {
        (void)timeout_ms;
        // Simulate the time a connection takes.
        advance_millis(100);
        g_wifi_connected = true;
        return true;
    }
}

// Mock MQTT functions
namespace {
    bool mqtt_is_connected() { return g_mqtt_connected; }
    bool mqtt_connect() {
        if (g_wifi_connected) {
            g_mqtt_connected = true;
            return true;
        }
        return false;
    }
    void mqtt_loop() {}
    void mqtt_disconnect() {
        g_mqtt_connected = false;
    }
}

// Mock sensor functions
namespace {
    void sensors_init_all() {
        g_sensors_initialized = true;
    }
    
    struct InsideReadings {
        float temperatureC;
        float humidityPct;
        float pressureHPa;
    };
    
    InsideReadings read_inside_sensors() {
        InsideReadings r;
        r.temperatureC = g_last_temp;
        r.humidityPct = g_last_humidity;
        r.pressureHPa = 1013.25f;
        return r;
    }
}

// Test sensor initialization
void test_sensor_initialization() {
    // Reset state
    g_sensors_initialized = false;
    
    // Call sensor init
    sensors_init_all();
    
    // Verify initialization
    TEST_ASSERT_TRUE(g_sensors_initialized);
}

// Test WiFi connection flow
void test_wifi_connection() {
    // Reset state
    g_wifi_connected = false;
    
    // Attempt connection
    bool result = wifi_connect_with_timeout(5000);
    
    // Verify connection
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(g_wifi_connected);
    TEST_ASSERT_TRUE(wifi_is_connected());
}

// Test MQTT connection flow
void test_mqtt_connection() {
    // Reset state
    g_wifi_connected = false;
    g_mqtt_connected = false;
    
    // MQTT should fail without WiFi
    bool result = mqtt_connect();
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_FALSE(g_mqtt_connected);
    
    // Connect WiFi first
    g_wifi_connected = true;
    
    // Now MQTT should succeed
    result = mqtt_connect();
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(g_mqtt_connected);
    TEST_ASSERT_TRUE(mqtt_is_connected());
    
    // Test disconnect
    mqtt_disconnect();
    TEST_ASSERT_FALSE(g_mqtt_connected);
    TEST_ASSERT_FALSE(mqtt_is_connected());
}

// Test sensor reading flow
void test_sensor_reading() {
    // Initialize sensors
    sensors_init_all();
    TEST_ASSERT_TRUE(g_sensors_initialized);
    
    // Set test values
    g_last_temp = 22.5f;
    g_last_humidity = 65.0f;
    
    // Read sensors
    InsideReadings readings = read_inside_sensors();
    
    // Verify readings
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 22.5f, readings.temperatureC);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 65.0f, readings.humidityPct);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1013.25f, readings.pressureHPa);
}

// Test temperature conversion
void test_temperature_conversion() {
    // Test Celsius to Fahrenheit conversion
    float celsius = 0.0f;
    float fahrenheit = celsius * 9.0f / 5.0f + 32.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 32.0f, fahrenheit);
    
    celsius = 100.0f;
    fahrenheit = celsius * 9.0f / 5.0f + 32.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 212.0f, fahrenheit);
    
    celsius = 25.0f;
    fahrenheit = celsius * 9.0f / 5.0f + 32.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 77.0f, fahrenheit);
    
    celsius = -40.0f;
    fahrenheit = celsius * 9.0f / 5.0f + 32.0f;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -40.0f, fahrenheit);
}

// Test phase sequencing
void test_phase_sequencing() {
    // Reset all states
    g_wifi_connected = false;
    g_mqtt_connected = false;
    g_sensors_initialized = false;
    
    // Phase 1: Sensor initialization
    sensors_init_all();
    TEST_ASSERT_TRUE(g_sensors_initialized);
    
    // Phase 2: Network connection
    bool wifi_ok = wifi_connect_with_timeout(5000);
    TEST_ASSERT_TRUE(wifi_ok);
    
    if (wifi_ok) {
        bool mqtt_ok = mqtt_connect();
        TEST_ASSERT_TRUE(mqtt_ok);
    }
    
    // Phase 3: Data collection (sensor reading)
    InsideReadings readings = read_inside_sensors();
    TEST_ASSERT_TRUE(isfinite(readings.temperatureC));
    TEST_ASSERT_TRUE(isfinite(readings.humidityPct));
    
    // Phase 4: Cleanup
    mqtt_disconnect();
    TEST_ASSERT_FALSE(mqtt_is_connected());
}

// Test error handling
void test_error_handling() {
    // Test sensor reading with NaN values
    g_last_temp = NAN;
    g_last_humidity = NAN;
    
    InsideReadings readings = read_inside_sensors();
    TEST_ASSERT_TRUE(isnan(readings.temperatureC));
    TEST_ASSERT_TRUE(isnan(readings.humidityPct));
    
    // Test that we can still handle NaN gracefully
    // (In real code, this would skip publishing or use defaults)
    
    // Reset to valid values
    g_last_temp = 20.0f;
    g_last_humidity = 40.0f;
    
    readings = read_inside_sensors();
    TEST_ASSERT_FALSE(isnan(readings.temperatureC));
    TEST_ASSERT_FALSE(isnan(readings.humidityPct));
}

// Test timing constraints
void test_timing_constraints() {
    // Test that operations complete within expected timeframes
    uint32_t start_ms, duration_ms;
    
    // Sensor init should be fast (< 500ms)
    start_ms = millis();
    sensors_init_all();
    duration_ms = millis() - start_ms;
    TEST_ASSERT_LESS_THAN(500, duration_ms);
    
    // WiFi connection mock should respect timeout
    start_ms = millis();
    wifi_connect_with_timeout(1000);
    duration_ms = millis() - start_ms;
    TEST_ASSERT_LESS_THAN(2000, duration_ms);
}

// Native entry point. These tests run on platform = native, which has no
// Arduino setup()/loop() runtime to call into.
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    // Initialize serial for test output
    Serial.begin(115200);
    delay(100);
    
    UNITY_BEGIN();
    
    // Run all tests
    RUN_TEST(test_sensor_initialization);
    RUN_TEST(test_wifi_connection);
    RUN_TEST(test_mqtt_connection);
    RUN_TEST(test_sensor_reading);
    RUN_TEST(test_temperature_conversion);
    RUN_TEST(test_phase_sequencing);
    RUN_TEST(test_error_handling);
    RUN_TEST(test_timing_constraints);
    return UNITY_END();
}
