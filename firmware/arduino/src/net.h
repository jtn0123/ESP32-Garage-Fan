#pragma once

// Network module - thin facade over the WiFi, MQTT, and HA discovery modules.
//
// This file used to carry a "compatibility wrapper" layer of ~20 net_publish_*
// inlines mirroring every mqtt_publish_* function; none of them had a caller
// (live publishing goes through MQTTBatcher in app_controller), so the layer
// and its orphaned backends were removed. Only the functions with real call
// sites remain.

#include <esp_mac.h>
#include <cstdio>
#include "wifi_manager.h"
#include "mqtt_client.h"
#include "ha_discovery.h"
#include "logging/log_mqtt.h"
#include "common_types.h"
#include "generated_config.h"

// Static storage for backward compatibility
static char g_client_id[40];
static bool g_diagnostic_mode_requested = false;
static bool g_diagnostic_mode_request_value = false;

// Initialize networking components. Idempotent: the always-on loop calls this
// from its link-check path so a node that booted before the router was up still
// gets MQTT wired once WiFi appears — without the guard, that late call would
// re-run mqtt_begin() and re-register callbacks every 30 s.
inline void net_begin() {
  static bool s_begun = false;
  if (s_begun)
    return;
  s_begun = true;

  // Generate client ID from the efuse factory MAC. esp_read_mac() needs no
  // WiFi state: on core 3.x, WiFi.macAddress() only works after the STA
  // interface starts, and calling it here left the id empty - every topic
  // (LWT, discovery, cmd/#, screenshots) landed under espsensor/unknown/ on
  // the first 3.x boot.
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
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

inline void net_ip_cstr(char* out, size_t out_size) { wifi_get_ip_cstr(out, out_size); }

inline void net_publish_wifi_rssi(int rssiDbm) { mqtt_publish_wifi_rssi(rssiDbm); }

inline void net_publish_debug_json(const char* payload, bool retain = false) {
  mqtt_publish_debug_json(payload, retain);
}

inline void net_publish_uptime(uint32_t uptime_sec) { mqtt_publish_uptime(uptime_sec); }

inline void net_publish_wake_count(uint32_t count) { mqtt_publish_wake_count(count); }

inline void net_publish_memory_diagnostics(uint32_t free_heap, uint32_t min_heap,
                                           uint32_t largest_block, float fragmentation_pct) {
  mqtt_publish_memory_diagnostics(free_heap, min_heap, largest_block, fragmentation_pct);
}

inline void net_publish_diagnostic_mode(bool active) { mqtt_publish_diagnostic_mode(active); }

// Home Assistant discovery wrapper
inline void net_publish_ha_discovery() { ha_discovery_publish_all(); }

// Ensure MQTT is connected (reconnect if needed)
inline void ensure_mqtt_connected() {
  if (!mqtt_is_connected() && wifi_is_connected()) {
    Serial.println(F("MQTT disconnected, reconnecting..."));
    mqtt_connect();
  }
}

inline OutsideReadings net_get_outside_readings() { return mqtt_get_outside_readings(); }

// Backward compatibility alias
inline OutsideReadings net_get_outside() { return net_get_outside_readings(); }

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
