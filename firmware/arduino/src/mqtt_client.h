#pragma once

// MQTT client module - extracted from net.h
// Handles MQTT connection and publishing

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include "common_types.h"

// MQTT connection parameters
#ifndef MQTT_CONNECT_TIMEOUT_MS
#define MQTT_CONNECT_TIMEOUT_MS 4000
#endif

#ifndef MQTT_MAX_PACKET_SIZE
#define MQTT_MAX_PACKET_SIZE 512
#endif

// MQTT client management
void mqtt_begin();
void mqtt_set_client_id(const char* client_id);
const char* mqtt_get_client_id();
void mqtt_loop();
bool mqtt_connect();
bool mqtt_is_connected();
void mqtt_disconnect();
void mqtt_set_server(const char* server, uint16_t port);
bool mqtt_publish_raw(const char* topic, const char* payload, bool retain);

// Publishing functions
void mqtt_publish_inside(float tempC, float rhPct);
void mqtt_publish_pressure(float pressureHPa);
void mqtt_publish_battery(float voltage, int percent);
void mqtt_publish_wifi_rssi(int rssiDbm);
void mqtt_publish_status(const char* payload, bool retain = true);
void mqtt_publish_debug_json(const char* payload, bool retain = false);
void mqtt_publish_last_crash(const char* reason_or_null);
void mqtt_publish_debug_probe(const char* payload, bool retain = false);
void mqtt_publish_boot_reason(const char* reason);
void mqtt_publish_boot_count(uint32_t count);
void mqtt_publish_crash_count(uint32_t count);
void mqtt_publish_uptime(uint32_t uptime_sec);
void mqtt_publish_wake_count(uint32_t count);
void mqtt_publish_memory_diagnostics(uint32_t free_heap, uint32_t min_heap, uint32_t largest_block,
                                     float fragmentation_pct);
void mqtt_publish_diagnostic_mode(bool active);
void mqtt_publish_publish_latency_ms(uint32_t publishLatencyMs);

// Outside readings management

// Access to MQTT client for logging
PubSubClient* mqtt_get_client();
OutsideReadings mqtt_get_outside_readings();

// Diagnostic mode
bool mqtt_is_diagnostic_mode_requested();
bool mqtt_get_diagnostic_mode_value();
void mqtt_clear_diagnostic_mode_request();
