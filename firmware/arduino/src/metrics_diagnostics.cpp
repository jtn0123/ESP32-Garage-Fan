// Metrics and diagnostics implementation
#include "metrics_diagnostics.h"
#include <time.h>
#include <cstdio>
#include "generated_config.h"
#if USE_DISPLAY
#include "display_layout.h"
#endif
#include "net.h"
#include "mqtt_client.h"
#include "safe_strings.h"  // Add safe string operations
#include "logging.h"       // Add logging infrastructure
#include "runtime_config.h"

// Error statistics (persist across deep sleep)
RTC_DATA_ATTR ErrorStats g_error_stats = {0};

// Global diagnostic mode flag
static bool g_diagnostic_mode = false;

// RTC variables for boot tracking (persist across deep sleep)
RTC_DATA_ATTR static uint32_t rtc_boot_count = 0;             // Total boots since power-on
RTC_DATA_ATTR static uint32_t rtc_crash_count = 0;            // Count of abnormal resets
RTC_DATA_ATTR static uint32_t rtc_cumulative_uptime_sec = 0;  // Total awake time in seconds
RTC_DATA_ATTR static uint32_t rtc_last_boot_timestamp = 0;    // Timestamp of last boot
RTC_DATA_ATTR static esp_reset_reason_t rtc_last_reset_reason = ESP_RST_UNKNOWN;
// Check if diagnostic mode is active
bool is_diagnostic_mode_active() { return g_diagnostic_mode; }

// Set diagnostic mode
void set_diagnostic_mode(bool active) {
  g_diagnostic_mode = active;
  mqtt_publish_diagnostic_mode(active);
}

// Error statistics functions
void increment_error_stat(const char* stat_name) {
  if (strcmp(stat_name, "mqtt_truncation") == 0) {
    g_error_stats.mqtt_payload_truncations++;
  } else if (strcmp(stat_name, "rtc_corruption") == 0) {
    g_error_stats.rtc_memory_corruptions++;
  } else if (strcmp(stat_name, "sensor_failure") == 0) {
    g_error_stats.sensor_read_failures++;
  } else if (strcmp(stat_name, "wifi_disconnect") == 0) {
    g_error_stats.wifi_disconnects++;
  } else if (strcmp(stat_name, "mqtt_publish_fail") == 0) {
    g_error_stats.mqtt_publish_failures++;
  }
}

void reset_error_stats() { memset(&g_error_stats, 0, sizeof(g_error_stats)); }

void publish_error_stats() {
  if (!mqtt_is_connected())
    return;

  char topic_buf[96];
  char payload[32];

  // Publish each stat
  snprintf(payload, sizeof(payload), "%lu", (unsigned long)g_error_stats.mqtt_payload_truncations);
  snprintf(topic_buf, sizeof(topic_buf), "espsensor/%s/debug/errors/truncations",
           mqtt_get_client_id());
  mqtt_publish_raw(topic_buf, payload, false);

  snprintf(payload, sizeof(payload), "%lu", (unsigned long)g_error_stats.rtc_memory_corruptions);
  snprintf(topic_buf, sizeof(topic_buf), "espsensor/%s/debug/errors/rtc_corruptions",
           mqtt_get_client_id());
  mqtt_publish_raw(topic_buf, payload, false);

  snprintf(payload, sizeof(payload), "%lu", (unsigned long)g_error_stats.sensor_read_failures);
  snprintf(topic_buf, sizeof(topic_buf), "espsensor/%s/debug/errors/sensor_failures",
           mqtt_get_client_id());
  mqtt_publish_raw(topic_buf, payload, false);

  snprintf(payload, sizeof(payload), "%lu", (unsigned long)g_error_stats.wifi_disconnects);
  snprintf(topic_buf, sizeof(topic_buf), "espsensor/%s/debug/errors/wifi_disconnects",
           mqtt_get_client_id());
  mqtt_publish_raw(topic_buf, payload, false);

  snprintf(payload, sizeof(payload), "%lu", (unsigned long)g_error_stats.mqtt_publish_failures);
  snprintf(topic_buf, sizeof(topic_buf), "espsensor/%s/debug/errors/mqtt_failures",
           mqtt_get_client_id());
  mqtt_publish_raw(topic_buf, payload, false);
}

uint32_t get_error_stat(const char* stat_name) {
  if (strcmp(stat_name, "mqtt_truncation") == 0) {
    return g_error_stats.mqtt_payload_truncations;
  } else if (strcmp(stat_name, "rtc_corruption") == 0) {
    return g_error_stats.rtc_memory_corruptions;
  } else if (strcmp(stat_name, "sensor_failure") == 0) {
    return g_error_stats.sensor_read_failures;
  } else if (strcmp(stat_name, "wifi_disconnect") == 0) {
    return g_error_stats.wifi_disconnects;
  } else if (strcmp(stat_name, "mqtt_publish_fail") == 0) {
    return g_error_stats.mqtt_publish_failures;
  }
  return 0;
}

// Check for rapid reset diagnostic trigger
// Uses RTC memory to track reset frequency across deep sleep cycles
bool check_rapid_reset_diagnostic_trigger() {
  // Check if we've had 3+ resets within a short time window
  // Uses RTC-persisted boot tracking variables

  constexpr uint32_t RAPID_RESET_THRESHOLD_SEC = 30;  // Rapid reset = within 30 seconds
  constexpr uint32_t RAPID_RESET_COUNT_TRIGGER = 3;   // Trigger after 3 rapid resets

  uint32_t current_time = static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);

  // Check if last boot was recent (rapid reset indicator)
  // Guard against clock reset or wrap: only compare if current >= last boot
  if (rtc_last_boot_timestamp > 0 && current_time >= rtc_last_boot_timestamp) {
    uint32_t time_since_last_boot = current_time - rtc_last_boot_timestamp;

    // Check both time-based and count-based triggers
    bool is_rapid_boot = (time_since_last_boot < RAPID_RESET_THRESHOLD_SEC);
    bool has_many_crashes = (rtc_crash_count >= RAPID_RESET_COUNT_TRIGGER);

    if (is_rapid_boot && has_many_crashes) {
      LOG_WARN("Rapid reset trigger: %u crashes in %u seconds", rtc_crash_count,
               time_since_last_boot);
      return true;
    }
  }

  return false;
}

// Emit metrics as JSON for debugging
void emit_metrics_json(float tempC, float rhPct, float pressHPa) {
  if (!mqtt_is_connected()) {
    return;
  }

  mqtt_payload_t json;  // Use pre-sized buffer type
  safe_snprintf(json, "{\"temp_c\":%.2f,\"rh_pct\":%.1f,\"press_hpa\":%.1f,\"ts\":%lu}",
                isfinite(tempC) ? tempC : 0.0f, isfinite(rhPct) ? rhPct : 0.0f,
                isfinite(pressHPa) ? pressHPa : 0.0f, (unsigned long)time(nullptr));

  mqtt_publish_debug_json(json, false);
}

// Publish layout identity for UI parity checking
void publish_layout_identity() {
  if (!mqtt_is_connected()) {
    return;
  }

  mqtt_topic_t topic;  // Use pre-sized buffer type
  safe_snprintf(topic, "%s/layout", rc_mqtt_pub_base());

  char payload[96];
#if USE_DISPLAY
  safe_snprintf(payload, "{\"layout_version\":%u,\"layout_crc\":\"0x%08X\"}",
                static_cast<unsigned>(LAYOUT_VERSION), static_cast<unsigned>(LAYOUT_CRC));
#else
  safe_snprintf(payload, "{\"layout_version\":%u,\"layout_crc\":\"%s\"}", 0u, "N/A");
#endif

  mqtt_publish_raw(topic, payload, true);
}

// Pump network for a duration to receive MQTT messages
void pump_network_ms(uint32_t duration_ms) {
  uint32_t start = millis();

  while (millis() - start < duration_ms) {
    net_loop();
    delay(10);
  }
}

// Get current time as HH:MM string
void net_time_hhmm(char* out, size_t out_size) {
  if (!out || out_size < 6) {
    return;
  }

  time_t now = time(nullptr);
  struct tm tm_now;
  localtime_r(&now, &tm_now);

  snprintf(out, out_size, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
}

// Boot and crash tracking functions
void update_boot_counters() {
  esp_reset_reason_t current_reset_reason = esp_reset_reason();
  rtc_last_reset_reason = current_reset_reason;

  // Update boot and crash counters
  if (current_reset_reason == ESP_RST_POWERON) {
    // Power-on reset: clear all counters
    rtc_boot_count = 1;
    rtc_crash_count = 0;
    rtc_cumulative_uptime_sec = 0;
  } else {
    // Any other reset: increment boot count
    rtc_boot_count++;

    // Increment crash count for abnormal resets
    // Need to check if it's a crash (would need system_manager function)
    if (current_reset_reason == ESP_RST_PANIC || current_reset_reason == ESP_RST_INT_WDT ||
        current_reset_reason == ESP_RST_TASK_WDT || current_reset_reason == ESP_RST_WDT ||
        current_reset_reason == ESP_RST_BROWNOUT) {
      rtc_crash_count++;
    }
  }

  // Update boot timestamp for rapid reset detection
  // Check for time() failure (returns -1 on error)
  time_t now = time(nullptr);
  if (now != (time_t)-1) {
    rtc_last_boot_timestamp = static_cast<uint32_t>(now);
  }
}

uint32_t get_boot_count() { return rtc_boot_count; }

uint32_t get_crash_count() { return rtc_crash_count; }

uint32_t get_cumulative_uptime_sec() { return rtc_cumulative_uptime_sec; }

void add_to_cumulative_uptime(uint32_t seconds) { rtc_cumulative_uptime_sec += seconds; }

uint32_t get_last_boot_timestamp() { return rtc_last_boot_timestamp; }

void set_last_boot_timestamp(uint32_t timestamp) { rtc_last_boot_timestamp = timestamp; }

esp_reset_reason_t get_last_reset_reason() { return rtc_last_reset_reason; }

void publish_boot_diagnostics() {
  if (!mqtt_is_connected()) {
    return;
  }

  // Publish boot diagnostics via MQTT
  char topic[128];
  char payload[64];

  snprintf(topic, sizeof(topic), "%s/debug/boot_count", rc_mqtt_pub_base());
  snprintf(payload, sizeof(payload), "%lu", (unsigned long)rtc_boot_count);
  mqtt_publish_raw(topic, payload, false);

  snprintf(topic, sizeof(topic), "%s/debug/crash_count", rc_mqtt_pub_base());
  snprintf(payload, sizeof(payload), "%lu", (unsigned long)rtc_crash_count);
  mqtt_publish_raw(topic, payload, false);

  snprintf(topic, sizeof(topic), "%s/debug/uptime", rc_mqtt_pub_base());
  snprintf(payload, sizeof(payload), "%lu", (unsigned long)rtc_cumulative_uptime_sec);
  mqtt_publish_raw(topic, payload, false);
}
