// Unit tests for power management module
// Tests battery estimation, sleep interval calculation, and device mode logic
//
// CAUTION: the functions below are hand-copied from src/power.cpp rather than
// compiled from it -- power.cpp pulls in Serial, Wire and the Adafruit fuel-gauge
// drivers, none of which exist on platform = native. That copy is free to drift
// from the real implementation, which is exactly what happened once already (see
// is_dev_mode). Any change to the device-mode or sleep-interval logic in
// src/power.cpp must be mirrored here.
//
// Prefer the pattern in src/config_merge.h + test/test_config_merge/ for new
// logic: put the pure part in a dependency-free header the native test can
// include directly, so no copy is possible.

#include <unity.h>

#include "../../src/power_pure.h"
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cmath>

// Mock millis() for testing time-based logic
static uint32_t g_mock_millis = 0;
uint32_t millis() { return g_mock_millis; }
void set_mock_millis(uint32_t ms) { g_mock_millis = ms; }
void advance_millis(uint32_t ms) { g_mock_millis += ms; }

// Constrain helper (matches Arduino)
template<typename T>
T constrain(T value, T min_val, T max_val) {
    if (value < min_val) return min_val;
    if (value > max_val) return max_val;
    return value;
}

// === Battery Estimation Functions (extracted from power.cpp) ===

int estimate_battery_percent(float voltage) {
    if (!std::isfinite(voltage) || voltage < 3.0f)
        return 0;
    if (voltage >= 4.2f)
        return 100;

    // Simplified LiPo discharge curve
    const float v_min = 3.2f;
    const float v_max = 4.2f;
    float normalized = (voltage - v_min) / (v_max - v_min);
    return constrain(static_cast<int>(normalized * 100), 0, 100);
}

int estimate_battery_days(int percent, float mah_capacity = 3000.0f, float ma_average = 50.0f) {
    if (percent < 0 || mah_capacity <= 0 || ma_average <= 0)
        return -1;

    float hours = (mah_capacity * percent / 100.0f) / ma_average;
    return static_cast<int>(hours / 24.0f);
}

// === Sleep Configuration ===

struct SleepConfig {
    uint32_t normal_interval_sec;
    uint32_t low_battery_interval_sec;
    uint32_t critical_interval_sec;
    uint32_t rapid_update_interval_sec;
    uint8_t low_battery_threshold;
    uint8_t critical_battery_threshold;
};

static SleepConfig g_sleep_config = {
    .normal_interval_sec = 300,           // 5 minutes
    .low_battery_interval_sec = 600,      // 10 minutes
    .critical_interval_sec = 1800,        // 30 minutes
    .rapid_update_interval_sec = 60,      // 1 minute
    .low_battery_threshold = 20,
    .critical_battery_threshold = 5
};

// === Device Mode Functions ===

// Device mode: exercised through the REAL implementation in src/power_pure.h,
// not a copy of it. That header takes the clock as a parameter precisely so this
// test can drive time without an Arduino runtime.
static DevModeState g_dev_mode;

void set_device_mode(const char* mode) { g_dev_mode.set(mode, millis()); }
bool is_dev_mode() { return g_dev_mode.is_active(millis()); }
uint32_t get_dev_mode_remaining_sec() { return g_dev_mode.remaining_sec(millis()); }
const char* get_device_mode_str() { return is_dev_mode() ? "dev" : "production"; }

// Custom sleep interval (set via MQTT, 0 = use adaptive). The floor comes from
// clamp_sleep_interval_sec() in power_pure.h, the same helper power.cpp uses.
static uint32_t g_custom_sleep_interval_sec = 0;

void set_custom_sleep_interval(uint32_t sec) {
    g_custom_sleep_interval_sec = clamp_sleep_interval_sec(sec);
}

uint32_t get_custom_sleep_interval() { return g_custom_sleep_interval_sec; }

// Mock battery status for testing
struct BatteryStatus {
    float voltage;
    int percent;
    int estimatedDays;
};

static BatteryStatus g_mock_battery = {4.0f, 80, 2};

BatteryStatus read_battery_status() {
    return g_mock_battery;
}

void set_mock_battery(float voltage, int percent) {
    g_mock_battery.voltage = voltage;
    g_mock_battery.percent = percent;
}

// Mock temperature change detection
static bool g_temp_changing_rapidly = false;

bool is_temperature_changing_rapidly() {
    return g_temp_changing_rapidly;
}

void set_temp_changing_rapidly(bool value) {
    g_temp_changing_rapidly = value;
}

// Sleep interval calculation (from power.cpp)
uint32_t calculate_optimal_sleep_interval(const SleepConfig& config) {
    if (g_custom_sleep_interval_sec > 0) {
        return g_custom_sleep_interval_sec;
    }

    BatteryStatus bs = read_battery_status();

    if (bs.percent >= 0 && bs.percent < config.critical_battery_threshold) {
        return config.critical_interval_sec;
    }

    if (bs.percent >= 0 && bs.percent < config.low_battery_threshold) {
        return config.low_battery_interval_sec;
    }

    if (is_temperature_changing_rapidly()) {
        return config.rapid_update_interval_sec;
    }

    return config.normal_interval_sec;
}

void setUp() {
    g_mock_millis = 0;
    g_dev_mode = DevModeState();
    g_custom_sleep_interval_sec = 0;
    g_mock_battery = {4.0f, 80, 2};
    g_temp_changing_rapidly = false;
}

void tearDown() {
    // Nothing to clean up
}

// === Battery Percent Estimation Tests ===

void test_battery_percent_full() {
    TEST_ASSERT_EQUAL(100, estimate_battery_percent(4.2f));
    TEST_ASSERT_EQUAL(100, estimate_battery_percent(4.3f));  // Above max
    TEST_ASSERT_EQUAL(100, estimate_battery_percent(5.0f));  // Way above max
}

void test_battery_percent_empty() {
    TEST_ASSERT_EQUAL(0, estimate_battery_percent(3.2f));
    TEST_ASSERT_EQUAL(0, estimate_battery_percent(3.0f));
    TEST_ASSERT_EQUAL(0, estimate_battery_percent(2.5f));  // Below min
}

void test_battery_percent_mid_range() {
    // 3.7V should be ~50%
    int pct = estimate_battery_percent(3.7f);
    TEST_ASSERT_TRUE(pct >= 45 && pct <= 55);

    // 3.5V should be ~30%
    pct = estimate_battery_percent(3.5f);
    TEST_ASSERT_TRUE(pct >= 25 && pct <= 35);

    // 4.0V should be ~80%
    pct = estimate_battery_percent(4.0f);
    TEST_ASSERT_TRUE(pct >= 75 && pct <= 85);
}

void test_battery_percent_invalid_input() {
    TEST_ASSERT_EQUAL(0, estimate_battery_percent(NAN));
    TEST_ASSERT_EQUAL(0, estimate_battery_percent(INFINITY));
    TEST_ASSERT_EQUAL(0, estimate_battery_percent(-INFINITY));
}

// === Battery Days Estimation Tests ===

void test_battery_days_full() {
    // 100% at 3000mAh / 50mA = 60 hours = 2.5 days
    int days = estimate_battery_days(100, 3000.0f, 50.0f);
    TEST_ASSERT_EQUAL(2, days);  // 2.5 days truncated to 2
}

void test_battery_days_half() {
    // 50% at 3000mAh / 50mA = 30 hours = 1.25 days
    int days = estimate_battery_days(50, 3000.0f, 50.0f);
    TEST_ASSERT_EQUAL(1, days);
}

void test_battery_days_zero() {
    TEST_ASSERT_EQUAL(0, estimate_battery_days(0, 3000.0f, 50.0f));
}

void test_battery_days_invalid_inputs() {
    TEST_ASSERT_EQUAL(-1, estimate_battery_days(-1, 3000.0f, 50.0f));
    TEST_ASSERT_EQUAL(-1, estimate_battery_days(50, 0.0f, 50.0f));
    TEST_ASSERT_EQUAL(-1, estimate_battery_days(50, 3000.0f, 0.0f));
    TEST_ASSERT_EQUAL(-1, estimate_battery_days(50, -1.0f, 50.0f));
    TEST_ASSERT_EQUAL(-1, estimate_battery_days(50, 3000.0f, -1.0f));
}

void test_battery_days_large_capacity() {
    // 10000mAh battery at 100% / 50mA = 200 hours = 8.3 days
    int days = estimate_battery_days(100, 10000.0f, 50.0f);
    TEST_ASSERT_EQUAL(8, days);
}

// === Custom Sleep Interval Tests ===

void test_custom_sleep_interval_minimum() {
    set_custom_sleep_interval(60);  // Below minimum
    TEST_ASSERT_EQUAL(180, get_custom_sleep_interval());  // Should clamp to 180

    set_custom_sleep_interval(179);  // Just below minimum
    TEST_ASSERT_EQUAL(180, get_custom_sleep_interval());

    set_custom_sleep_interval(180);  // Exactly minimum
    TEST_ASSERT_EQUAL(180, get_custom_sleep_interval());
}

void test_custom_sleep_interval_normal() {
    set_custom_sleep_interval(300);
    TEST_ASSERT_EQUAL(300, get_custom_sleep_interval());

    set_custom_sleep_interval(600);
    TEST_ASSERT_EQUAL(600, get_custom_sleep_interval());

    set_custom_sleep_interval(3600);
    TEST_ASSERT_EQUAL(3600, get_custom_sleep_interval());
}

void test_custom_sleep_interval_zero_clears() {
    set_custom_sleep_interval(600);
    TEST_ASSERT_EQUAL(600, get_custom_sleep_interval());

    // Setting to 0 should clamp to 180 (minimum)
    set_custom_sleep_interval(0);
    TEST_ASSERT_EQUAL(180, get_custom_sleep_interval());
}

// === Device Mode Tests ===

void test_device_mode_default() {
    TEST_ASSERT_FALSE(is_dev_mode());
    TEST_ASSERT_EQUAL_STRING("production", get_device_mode_str());
}

void test_device_mode_set_dev() {
    set_device_mode("dev");
    TEST_ASSERT_TRUE(is_dev_mode());
    TEST_ASSERT_EQUAL_STRING("dev", get_device_mode_str());
}

void test_device_mode_set_development() {
    set_device_mode("development");
    TEST_ASSERT_TRUE(is_dev_mode());
    TEST_ASSERT_EQUAL_STRING("dev", get_device_mode_str());
}

void test_device_mode_set_production() {
    set_device_mode("dev");
    TEST_ASSERT_TRUE(is_dev_mode());

    set_device_mode("production");
    TEST_ASSERT_FALSE(is_dev_mode());
    TEST_ASSERT_EQUAL_STRING("production", get_device_mode_str());
}

void test_device_mode_auto_timeout() {
    set_mock_millis(0);
    set_device_mode("dev");
    TEST_ASSERT_TRUE(is_dev_mode());

    // Just before timeout
    set_mock_millis(DEV_MODE_TIMEOUT_MS - 1);
    TEST_ASSERT_TRUE(is_dev_mode());

    // At timeout
    set_mock_millis(DEV_MODE_TIMEOUT_MS);
    TEST_ASSERT_FALSE(is_dev_mode());
    TEST_ASSERT_EQUAL_STRING("production", get_device_mode_str());
}

void test_device_mode_remaining_time() {
    set_mock_millis(0);
    set_device_mode("dev");

    uint32_t remaining = get_dev_mode_remaining_sec();
    TEST_ASSERT_EQUAL(3600, remaining);  // 1 hour

    set_mock_millis(1800000);  // 30 minutes
    remaining = get_dev_mode_remaining_sec();
    TEST_ASSERT_EQUAL(1800, remaining);  // 30 minutes left

    set_mock_millis(DEV_MODE_TIMEOUT_MS);
    remaining = get_dev_mode_remaining_sec();
    TEST_ASSERT_EQUAL(0, remaining);
}

void test_device_mode_remaining_when_production() {
    set_device_mode("production");
    TEST_ASSERT_EQUAL(0, get_dev_mode_remaining_sec());
}

// === Sleep Interval Calculation Tests ===

void test_sleep_interval_normal() {
    set_mock_battery(4.0f, 80);
    set_temp_changing_rapidly(false);
    g_custom_sleep_interval_sec = 0;

    uint32_t interval = calculate_optimal_sleep_interval(g_sleep_config);
    TEST_ASSERT_EQUAL(300, interval);  // Normal interval
}

void test_sleep_interval_low_battery() {
    set_mock_battery(3.5f, 15);  // Below 20% threshold
    set_temp_changing_rapidly(false);
    g_custom_sleep_interval_sec = 0;

    uint32_t interval = calculate_optimal_sleep_interval(g_sleep_config);
    TEST_ASSERT_EQUAL(600, interval);  // Low battery interval
}

void test_sleep_interval_critical_battery() {
    set_mock_battery(3.3f, 3);  // Below 5% threshold
    set_temp_changing_rapidly(false);
    g_custom_sleep_interval_sec = 0;

    uint32_t interval = calculate_optimal_sleep_interval(g_sleep_config);
    TEST_ASSERT_EQUAL(1800, interval);  // Critical battery interval
}

void test_sleep_interval_rapid_temp_change() {
    set_mock_battery(4.0f, 80);
    set_temp_changing_rapidly(true);
    g_custom_sleep_interval_sec = 0;

    uint32_t interval = calculate_optimal_sleep_interval(g_sleep_config);
    TEST_ASSERT_EQUAL(60, interval);  // Rapid update interval
}

void test_sleep_interval_custom_overrides_adaptive() {
    set_mock_battery(4.0f, 80);
    set_temp_changing_rapidly(false);
    set_custom_sleep_interval(900);

    uint32_t interval = calculate_optimal_sleep_interval(g_sleep_config);
    TEST_ASSERT_EQUAL(900, interval);  // Custom interval takes precedence
}

void test_sleep_interval_critical_battery_overrides_temp_change() {
    // Critical battery should take precedence over rapid temp changes
    set_mock_battery(3.3f, 3);  // Critical battery
    set_temp_changing_rapidly(true);
    g_custom_sleep_interval_sec = 0;

    uint32_t interval = calculate_optimal_sleep_interval(g_sleep_config);
    TEST_ASSERT_EQUAL(1800, interval);  // Critical battery wins
}

void test_sleep_interval_custom_overrides_critical_battery() {
    // Custom interval should override even critical battery
    set_mock_battery(3.3f, 3);
    set_temp_changing_rapidly(false);
    set_custom_sleep_interval(300);

    uint32_t interval = calculate_optimal_sleep_interval(g_sleep_config);
    TEST_ASSERT_EQUAL(300, interval);  // Custom wins
}

// === Edge Cases ===

void test_battery_percent_boundary_values() {
    // Test exact boundary values
    TEST_ASSERT_EQUAL(0, estimate_battery_percent(3.2f));
    TEST_ASSERT_EQUAL(100, estimate_battery_percent(4.2f));

    // Test just inside boundaries
    int pct = estimate_battery_percent(3.21f);
    TEST_ASSERT_TRUE(pct >= 0 && pct <= 5);

    pct = estimate_battery_percent(4.19f);
    TEST_ASSERT_TRUE(pct >= 95 && pct <= 100);
}

void test_sleep_interval_at_threshold_boundaries() {
    g_custom_sleep_interval_sec = 0;
    set_temp_changing_rapidly(false);

    // Exactly at critical threshold (5%)
    set_mock_battery(3.3f, 5);
    uint32_t interval = calculate_optimal_sleep_interval(g_sleep_config);
    TEST_ASSERT_EQUAL(600, interval);  // Should be low, not critical

    // Just below critical threshold (4%)
    set_mock_battery(3.3f, 4);
    interval = calculate_optimal_sleep_interval(g_sleep_config);
    TEST_ASSERT_EQUAL(1800, interval);  // Critical

    // Exactly at low threshold (20%)
    set_mock_battery(3.6f, 20);
    interval = calculate_optimal_sleep_interval(g_sleep_config);
    TEST_ASSERT_EQUAL(300, interval);  // Should be normal, not low

    // Just below low threshold (19%)
    set_mock_battery(3.6f, 19);
    interval = calculate_optimal_sleep_interval(g_sleep_config);
    TEST_ASSERT_EQUAL(600, interval);  // Low battery
}

// === ProbeRetry (src/power_pure.h, compiled directly - no hand copy) ===

// The first probe must always happen, including at millis() == 0. A gate keyed
// on "last_attempt_ms == 0 means never tried" would pass this by accident and
// then fail test_probe_retry_first_attempt_at_time_zero below.
void test_probe_retry_first_attempt_is_due(void) {
    ProbeRetry r;
    TEST_ASSERT_TRUE(r.due(5000));
}

void test_probe_retry_first_attempt_at_time_zero(void) {
    ProbeRetry r;
    TEST_ASSERT_TRUE(r.due(0));
    // Still rate limited afterwards, even though last_attempt_ms is 0.
    TEST_ASSERT_FALSE(r.due(1));
    TEST_ASSERT_FALSE(r.due(59999));
    TEST_ASSERT_TRUE(r.due(60000));
}

void test_probe_retry_rate_limits_within_interval(void) {
    ProbeRetry r;
    TEST_ASSERT_TRUE(r.due(1000));
    TEST_ASSERT_FALSE(r.due(1001));
    TEST_ASSERT_FALSE(r.due(30000));
    TEST_ASSERT_FALSE(r.due(60999));
}

// The regression this guards: a one-shot probe meant a single failed fuel-gauge
// begin() left the battery reading dead until a power cycle.
void test_probe_retry_retries_after_interval(void) {
    ProbeRetry r;
    TEST_ASSERT_TRUE(r.due(1000));
    TEST_ASSERT_TRUE(r.due(61000));
    TEST_ASSERT_FALSE(r.due(61001));
    TEST_ASSERT_TRUE(r.due(121000));
}

void test_probe_retry_honors_custom_interval(void) {
    ProbeRetry r;
    TEST_ASSERT_TRUE(r.due(0, 100));
    TEST_ASSERT_FALSE(r.due(99, 100));
    TEST_ASSERT_TRUE(r.due(100, 100));
}

// Unsigned subtraction must carry the gate across the ~49-day millis() wrap
// rather than stalling it for another full epoch. The casts are the point of the
// test: they are the wrap millis() itself performs, so writing them out keeps
// the arithmetic identical on a 64-bit host and a 32-bit target.
void test_probe_retry_survives_millis_rollover(void) {
    ProbeRetry r;
    const uint32_t near_max = 0xFFFFFFFFUL - 1000UL;
    TEST_ASSERT_TRUE(r.due(near_max));
    // Still inside the interval.
    TEST_ASSERT_FALSE(r.due(static_cast<uint32_t>(near_max + 500UL)));
    // Wrapped past it: the clock now reads 58999, far below last_attempt_ms.
    TEST_ASSERT_TRUE(r.due(static_cast<uint32_t>(near_max + 60000UL)));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Battery percent estimation
    RUN_TEST(test_battery_percent_full);
    RUN_TEST(test_battery_percent_empty);
    RUN_TEST(test_battery_percent_mid_range);
    RUN_TEST(test_battery_percent_invalid_input);

    // Battery days estimation
    RUN_TEST(test_battery_days_full);
    RUN_TEST(test_battery_days_half);
    RUN_TEST(test_battery_days_zero);
    RUN_TEST(test_battery_days_invalid_inputs);
    RUN_TEST(test_battery_days_large_capacity);

    // Custom sleep interval
    RUN_TEST(test_custom_sleep_interval_minimum);
    RUN_TEST(test_custom_sleep_interval_normal);
    RUN_TEST(test_custom_sleep_interval_zero_clears);

    // Device mode
    RUN_TEST(test_device_mode_default);
    RUN_TEST(test_device_mode_set_dev);
    RUN_TEST(test_device_mode_set_development);
    RUN_TEST(test_device_mode_set_production);
    RUN_TEST(test_device_mode_auto_timeout);
    RUN_TEST(test_device_mode_remaining_time);
    RUN_TEST(test_device_mode_remaining_when_production);

    // Sleep interval calculation
    RUN_TEST(test_sleep_interval_normal);
    RUN_TEST(test_sleep_interval_low_battery);
    RUN_TEST(test_sleep_interval_critical_battery);
    RUN_TEST(test_sleep_interval_rapid_temp_change);
    RUN_TEST(test_sleep_interval_custom_overrides_adaptive);
    RUN_TEST(test_sleep_interval_critical_battery_overrides_temp_change);
    RUN_TEST(test_sleep_interval_custom_overrides_critical_battery);

    // Fuel-gauge probe retry gate
    RUN_TEST(test_probe_retry_first_attempt_is_due);
    RUN_TEST(test_probe_retry_first_attempt_at_time_zero);
    RUN_TEST(test_probe_retry_rate_limits_within_interval);
    RUN_TEST(test_probe_retry_retries_after_interval);
    RUN_TEST(test_probe_retry_honors_custom_interval);
    RUN_TEST(test_probe_retry_survives_millis_rollover);

    // Edge cases
    RUN_TEST(test_battery_percent_boundary_values);
    RUN_TEST(test_sleep_interval_at_threshold_boundaries);

    return UNITY_END();
}
