#pragma once

// Network module - refactored to use separate WiFi, MQTT, and HA discovery modules
// This file now serves as a compatibility wrapper for the refactored modules

#include <cstdio>
#include "wifi_manager.h"
#include "mqtt_client.h"
#include "ha_discovery.h"
#include "logging/log_mqtt.h"
#include "common_types.h"
#include "generated_config.h"

// Static storage for backward compatibility
static char g_client_id[40];
static OutsideReadings g_outside;
static bool g_diagnostic_mode_requested = false;
static bool g_diagnostic_mode_request_value = false;

// Offline sample structure for data persistence
struct OfflineSample {
  uint32_t timestamp;
  float tempC;
  float rhPct;
  float pressureHPa;
  bool hasTemp;
  bool hasRh;
  bool hasPressure;
};

// Initialize networking components. Idempotent: the always-on loop calls this
// from its link-check path so a node that booted before the router was up still
// gets MQTT wired once WiFi appears — without the guard, that late call would
// re-run mqtt_begin() and re-register callbacks every 30 s.
inline void net_begin() {
  static bool s_begun = false;
  if (s_begun)
    return;
  s_begun = true;

  // Generate client ID
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(g_client_id, sizeof(g_client_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);

  // The id must be registered before mqtt_begin(): the first connect builds its
  // LWT and discovery topics immediately, and with the id still unset those all
  // land under espsensor/unknown/ where nothing is subscribed. The MQTT log
  // sink builds espsensor/<id>/logs/# topics the same way.
  mqtt_set_client_id(g_client_id);
  ha_discovery_begin(g_client_id);
  LogMQTT::getInstance()->setClientId(g_client_id);
  mqtt_begin();
}

// Main network loop
inline void net_loop() {
  mqtt_loop();

  // Check for diagnostic mode commands
  if (mqtt_is_diagnostic_mode_requested()) {
    g_diagnostic_mode_requested = true;
    g_diagnostic_mode_request_value = mqtt_get_diagnostic_mode_value();
    mqtt_clear_diagnostic_mode_request();
  }
}

// WiFi wrapper functions
inline bool net_wifi_is_connected() { return wifi_is_connected(); }

inline String net_ip() { return wifi_get_ip(); }

inline void net_ip_cstr(char* out, size_t out_size) { wifi_get_ip_cstr(out, out_size); }

inline bool net_wifi_clear_provisioning() { return wifi_clear_provisioning(); }

// MQTT wrapper functions
inline void net_publish_inside(float tempC, float rhPct) { mqtt_publish_inside(tempC, rhPct); }

inline void net_publish_pressure(float pressureHPa) { mqtt_publish_pressure(pressureHPa); }

inline void net_publish_battery(float voltage, int percent) {
  mqtt_publish_battery(voltage, percent);
}

inline void net_publish_wifi_rssi(int rssiDbm) { mqtt_publish_wifi_rssi(rssiDbm); }

inline void net_publish_status(const char* payload, bool retain = true) {
  mqtt_publish_status(payload, retain);
}

inline void net_publish_debug_json(const char* payload, bool retain = false) {
  mqtt_publish_debug_json(payload, retain);
}

inline void net_publish_last_crash(const char* reason_or_null) {
  mqtt_publish_last_crash(reason_or_null);
}

inline void net_publish_debug_probe(const char* payload, bool retain = false) {
  mqtt_publish_debug_probe(payload, retain);
}

inline void net_publish_boot_reason(const char* reason) { mqtt_publish_boot_reason(reason); }

inline void net_publish_boot_count(uint32_t count) { mqtt_publish_boot_count(count); }

inline void net_publish_crash_count(uint32_t count) { mqtt_publish_crash_count(count); }

inline void net_publish_uptime(uint32_t uptime_sec) { mqtt_publish_uptime(uptime_sec); }

inline void net_publish_wake_count(uint32_t count) { mqtt_publish_wake_count(count); }

inline void net_publish_memory_diagnostics(uint32_t free_heap, uint32_t min_heap,
                                           uint32_t largest_block, float fragmentation_pct) {
  mqtt_publish_memory_diagnostics(free_heap, min_heap, largest_block, fragmentation_pct);
}

inline void net_publish_diagnostic_mode(bool active) { mqtt_publish_diagnostic_mode(active); }

inline void net_publish_publish_latency_ms(uint32_t publishLatencyMs) {
  mqtt_publish_publish_latency_ms(publishLatencyMs);
}

// Home Assistant discovery wrapper
inline void net_publish_ha_discovery() { ha_discovery_publish_all(); }

// WiFi connection with retry logic
inline bool net_wifi_connect_with_retry(uint32_t timeout_ms, uint32_t max_attempts = 3) {
  for (uint32_t attempt = 0; attempt < max_attempts; attempt++) {
    if (wifi_connect_with_timeout(timeout_ms)) {
      return true;
    }
    delay(500);
  }
  return false;
}

// MQTT connection with retry logic
inline bool net_mqtt_connect_with_retry(uint32_t timeout_ms, uint32_t max_attempts = 3) {
  uint32_t start = millis();

  while ((millis() - start) < (timeout_ms * max_attempts)) {
    if (mqtt_connect()) {
      return true;
    }
    delay(1000);
  }
  return false;
}

// Full network initialization
inline bool net_init_and_connect() {
  // Connect to WiFi
  if (!net_wifi_connect_with_retry(WIFI_CONNECT_TIMEOUT_MS)) {
    Serial.println(F("WiFi connection failed"));
    return false;
  }

  Serial.print(F("WiFi connected, IP: "));
  Serial.println(wifi_get_ip());

  // Connect to MQTT
  if (!net_mqtt_connect_with_retry(MQTT_CONNECT_TIMEOUT_MS)) {
    Serial.println(F("MQTT connection failed"));
    return false;
  }

  Serial.println(F("MQTT connected"));

  // Publish discovery
  net_publish_ha_discovery();

  return true;
}

// Ensure WiFi is connected (reconnect if needed)
inline void ensure_wifi_connected() {
  if (!wifi_is_connected()) {
    Serial.println(F("WiFi disconnected, reconnecting..."));
    wifi_connect_with_timeout(WIFI_CONNECT_TIMEOUT_MS);
  }
}

// Ensure MQTT is connected (reconnect if needed)
inline void ensure_mqtt_connected() {
  if (!mqtt_is_connected() && wifi_is_connected()) {
    Serial.println(F("MQTT disconnected, reconnecting..."));
    mqtt_connect();
  }
}

// Outside readings management (for backward compatibility)
inline void net_set_outside_readings(const OutsideReadings& readings) {
  g_outside = readings;
  mqtt_update_outside_readings(readings);
}

inline OutsideReadings net_get_outside_readings() { return mqtt_get_outside_readings(); }

// Backward compatibility alias
inline OutsideReadings net_get_outside() { return net_get_outside_readings(); }

// Additional compatibility functions
inline bool net_mqtt_is_connected() { return mqtt_is_connected(); }

inline void net_prepare_for_sleep() {
  // Disconnect cleanly before sleep
  mqtt_disconnect();
}

inline bool net_check_diagnostic_mode_request(bool& value) {
  if (g_diagnostic_mode_requested) {
    value = g_diagnostic_mode_request_value;
    g_diagnostic_mode_requested = false;
    return true;
  }
  return false;
}
