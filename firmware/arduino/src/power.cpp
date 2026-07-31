// Power management implementation
// Copyright 2024 Justin

#include "power.h"
#include "power_pure.h"
#include <Wire.h>
#include <cmath>  // For fabsf()

#if USE_MAX17048
#include <Adafruit_MAX1704X.h>
static Adafruit_MAX17048 g_maxfg;
static bool g_maxfg_initialized = false;
#endif

#if USE_LC709203F
#include <Adafruit_LC709203F.h>
static Adafruit_LC709203F g_lcfg;
static bool g_lcfg_initialized = false;
#endif

// Power rail control pins (if defined)
static void enable_i2c_power() {
#ifdef PIN_I2C_POWER
  pinMode(PIN_I2C_POWER, OUTPUT);
  digitalWrite(PIN_I2C_POWER, HIGH);
#endif
#ifdef I2C_POWER
  pinMode(I2C_POWER, OUTPUT);
  digitalWrite(I2C_POWER, HIGH);
#endif
#ifdef TFT_I2C_POWER
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
#endif
}

static void ensure_i2c_initialized() {
#if defined(SDA) && defined(SCL)
  Wire.begin(SDA, SCL);
#else
  Wire.begin();
#endif
#ifdef I2C_TIMEOUT_MS
  // Use configured timeout if > 0, otherwise use safe default (50ms)
  Wire.setTimeOut(I2C_TIMEOUT_MS > 0 ? I2C_TIMEOUT_MS : 50);
#endif
#ifdef I2C_CLOCK_HZ
  Wire.setClock(I2C_CLOCK_HZ);
#endif
}

#if USE_MAX17048
void fuelgauge_wake_if_asleep() {
  if (!g_maxfg_initialized)
    return;
  g_maxfg.sleep(false);
}

void fuelgauge_quickstart_if_cold_boot(esp_reset_reason_t reason) {
  if (!g_maxfg_initialized)
    return;
  if (reason == ESP_RST_POWERON) {
    g_maxfg.quickStart();
  }
}

void fuelgauge_sleep_between_wakes() {
  if (!g_maxfg_initialized)
    return;
  g_maxfg.sleep(true);
}
#endif

#if USE_LC709203F
void lc_wake_if_asleep() { /* LC709203F has no sleep API */ }

void lc_quickstart_if_cold_boot(esp_reset_reason_t reason) {
  (void)reason;
  // LC709203F does not expose quickstart; ensure it is configured
}

void lc_sleep_between_wakes() { /* no-op */ }
#endif

// Average draw for the days-remaining estimate. Always-on is a measured-ish
// constant; the deep-sleep build duty-cycles between sleep and active current.
static float average_current_ma() {
#if ALWAYS_ON
  return ALWAYS_ON_AVG_CURRENT_MA;
#else
  return SLEEP_CURRENT_MA +
         (ACTIVE_CURRENT_MA * ACTIVE_SECONDS) / static_cast<float>(WAKE_INTERVAL_SEC);
#endif
}

BatteryStatus read_battery_status() {
  BatteryStatus b;

#if USE_MAX17048
  static bool s_maxfg_attempted = false;
  if (!g_maxfg_initialized && !s_maxfg_attempted) {
    enable_i2c_power();
    ensure_i2c_initialized();
    s_maxfg_attempted = true;

    if (g_maxfg.begin()) {
      g_maxfg_initialized = true;
      Serial.println("MAX17048 fuel gauge found");
      g_maxfg.setAlertVoltages(2.0, 4.2);
      uint8_t vers = g_maxfg.getChipID();
      Serial.printf("MAX17048 version: 0x%02X\n", vers);
    } else {
      Serial.println("MAX17048 not found");
    }
  }

  if (g_maxfg_initialized) {
    b.voltage = g_maxfg.cellVoltage();
    float pct = g_maxfg.cellPercent();
    b.percent = constrain(static_cast<int>(pct), 0, 100);

    if (b.percent >= 0) {
      // Was hardcoded (percent * 2.5)/100 — a fiction for any pack but the
      // 3000 mAh / 50 mA one in the old comment. Use the real capacity and the
      // build's actual duty cycle.
      b.estimatedDays =
          estimate_battery_days(b.percent, BATTERY_CAPACITY_MAH, average_current_ma());
    }
  }
#endif

#if USE_LC709203F
  static bool s_lcfg_attempted = false;
  if (!g_lcfg_initialized && !s_lcfg_attempted) {
    enable_i2c_power();
    ensure_i2c_initialized();
    s_lcfg_attempted = true;

    if (g_lcfg.begin()) {
      g_lcfg_initialized = true;
      Serial.println("LC709203F fuel gauge found");
      g_lcfg.setPowerMode(LC709203F_POWER_OPERATE);
      // APA for the actual pack: two 3300 mAh 18650s in parallel = 6600 mAh.
      // The library's named constants stop at 3000 mAh (0x36); the datasheet
      // table is close to linear at ~9 counts per 1000 mAh above 1000, so
      // 6600 mAh extrapolates to ~0x56. With the old 3000 mAh setting the
      // reported percentage sagged noticeably mid-discharge on this pack.
      g_lcfg.setPackAPA(BATTERY_APA);
      g_lcfg.setAlarmVoltage(3.4);
    } else {
      Serial.println("LC709203F not found");
    }
  }

  if (g_lcfg_initialized && !g_maxfg_initialized) {
    b.voltage = g_lcfg.cellVoltage();
    float pct = g_lcfg.cellPercent();
    b.percent = constrain(static_cast<int>(pct), 0, 100);

    if (b.percent >= 0) {
      b.estimatedDays =
          estimate_battery_days(b.percent, BATTERY_CAPACITY_MAH, average_current_ma());
    }
  }
#endif

  return b;
}

// Battery percentage from voltage
int estimate_battery_percent(float voltage) {
  if (!isfinite(voltage) || voltage < 3.0)
    return 0;
  if (voltage >= 4.2)
    return 100;

  // Simplified LiPo discharge curve
  const float v_min = 3.2;
  const float v_max = 4.2;
  float normalized = (voltage - v_min) / (v_max - v_min);
  return constrain(static_cast<int>(normalized * 100), 0, 100);
}

// Days remaining estimate
int estimate_battery_days(int percent, float mah_capacity, float ma_average) {
  if (percent < 0 || mah_capacity <= 0 || ma_average <= 0)
    return -1;

  float hours = (mah_capacity * percent / 100.0) / ma_average;
  return static_cast<int>(hours / 24.0);
}

// Initialize power management
void power_init() {
  Serial.println("Initializing power management...");

  // Enable power rails
  enable_i2c_power();

  // Initialize fuel gauge
  BatteryStatus initial = read_battery_status();
  if (initial.percent >= 0) {
    Serial.printf("Battery: %.2fV, %d%%, ~%d days\n", initial.voltage, initial.percent,
                  initial.estimatedDays);
  } else {
    Serial.println("No battery gauge found");
  }

  // Configure wakeup sources if needed
  // Wake interval will be configured when going to sleep
}

// Prepare for deep sleep
void power_prepare_sleep() {
#if USE_MAX17048
  fuelgauge_sleep_between_wakes();
#endif
#if USE_LC709203F
  lc_sleep_between_wakes();
#endif
}

// Wake from deep sleep
void power_wake_from_sleep() {
#if USE_MAX17048
  fuelgauge_wake_if_asleep();
  fuelgauge_quickstart_if_cold_boot(esp_reset_reason());
#endif
#if USE_LC709203F
  lc_wake_if_asleep();
  lc_quickstart_if_cold_boot(esp_reset_reason());
#endif
}

// Adaptive sleep scheduling implementation
#include "state_manager.h"

static float g_last_temperature = NAN;

// Custom sleep interval (set via MQTT, 0 = use adaptive)
static uint32_t g_custom_sleep_interval_sec = 0;

// Dev-mode state machine lives in power_pure.h so the native unit tests exercise
// this exact code instead of a hand-copied duplicate.
static DevModeState g_dev_mode;

void set_custom_sleep_interval(uint32_t sec) {
  // Enforce a minimum cadence to prevent sensor self-heating.
  g_custom_sleep_interval_sec = clamp_sleep_interval_sec(sec);
}

uint32_t get_custom_sleep_interval() { return g_custom_sleep_interval_sec; }

void set_device_mode(const char* mode) {
  g_dev_mode.set(mode, millis());
  Serial.println(g_dev_mode.active ? "[Power] Device mode: DEVELOPMENT (1hr timeout)"
                                   : "[Power] Device mode: PRODUCTION");
}

bool is_dev_mode() {
  bool was_active = g_dev_mode.active;
  bool active = g_dev_mode.is_active(millis());
  if (was_active && !active) {
    Serial.println("[Power] Dev mode auto-expired, reverting to production");
  }
  return active;
}

uint32_t get_dev_mode_remaining_sec() { return g_dev_mode.remaining_sec(millis()); }

const char* get_device_mode_str() { return is_dev_mode() ? "dev" : "production"; }

static SleepConfig g_sleep_config = {.normal_interval_sec = 300,       // 5 minutes
                                     .low_battery_interval_sec = 600,  // 10 minutes
                                     .critical_interval_sec = 1800,    // 30 minutes
                                     .rapid_update_interval_sec = 60,  // 1 minute
                                     .low_battery_threshold = 20,
                                     .critical_battery_threshold = 5};

SleepConfig get_default_sleep_config() { return g_sleep_config; }

bool is_temperature_changing_rapidly() {
  // Get current inside temperature from state
  float current_temp = get_last_inside_f();

  if (isnan(g_last_temperature) || isnan(current_temp)) {
    g_last_temperature = current_temp;
    return false;
  }

  // Use fabsf() for float absolute value (abs() returns int, truncating the delta)
  float delta = fabsf(current_temp - g_last_temperature);
  g_last_temperature = current_temp;

  return delta > 2.0f;  // More than 2°F change
}

uint32_t calculate_optimal_sleep_interval(const SleepConfig& config) {
  // If custom interval is set via MQTT, use it (overrides adaptive)
  if (g_custom_sleep_interval_sec > 0) {
    Serial.printf("[Power] Using custom sleep interval: %us\n", g_custom_sleep_interval_sec);
    return g_custom_sleep_interval_sec;
  }

  BatteryStatus bs = read_battery_status();

  // Critical battery - maximum conservation
  if (bs.percent >= 0 && bs.percent < config.critical_battery_threshold) {
    Serial.printf("[Power] Critical battery (%d%%), using %us interval\n", bs.percent,
                  config.critical_interval_sec);
    return config.critical_interval_sec;
  }

  // Low battery - extended interval
  if (bs.percent >= 0 && bs.percent < config.low_battery_threshold) {
    Serial.printf("[Power] Low battery (%d%%), using %us interval\n", bs.percent,
                  config.low_battery_interval_sec);
    return config.low_battery_interval_sec;
  }

  // Rapid temperature change - shorter interval for responsiveness
  if (is_temperature_changing_rapidly()) {
    Serial.println("[Power] Temperature changing rapidly, using short interval");
    return config.rapid_update_interval_sec;
  }

  // Normal operation
  return config.normal_interval_sec;
}
