#pragma once
// Copyright 2024 Justin
//
// Runtime configuration layer.
//
// Historically every setting was a compile-time macro from generated_config.h,
// which meant changing the room name or an MQTT host required a rebuild and a
// reflash. This module holds the *effective* configuration: it starts as a copy
// of those macros and can then be overlaid by /config/device.json on the
// microSD card. Consumers read from here instead of the macros so a value can
// come from either source without them caring which.
//
// Fields left blank in the card's JSON keep their compiled-in value (see
// config_merge.h), so a partially-filled or malformed card degrades to exactly
// the old behaviour.

#include <Arduino.h>
#include <cstdint>

struct RuntimeConfig {
  char room_name[33];
  char wifi_ssid[33];
  char wifi_pass[65];
  char mqtt_host[65];
  uint16_t mqtt_port;
  char mqtt_user[33];
  char mqtt_pass[65];
  // Bounded well below MQTT_TOPIC_SIZE (128) on purpose: these bases have topic
  // suffixes appended ("/debug/boot_count" is the longest at 17 chars), and a
  // base long enough to force truncation would silently publish to a topic
  // nobody is subscribed to.
  char mqtt_pub_base[65];
  char mqtt_sub_base[65];
  char ota_password[33];
  bool ota_enabled;
  uint32_t sample_interval_sec;
  bool history_enabled;
  bool logs_enabled;
  uint16_t history_retention_days;
};

// Bounds shared with the validator and the MQTT command handler.
#define RC_MIN_SAMPLE_INTERVAL_SEC 60
#define RC_MAX_SAMPLE_INTERVAL_SEC 3600
#define RC_DEFAULT_SAMPLE_INTERVAL_SEC 300

// Populate the effective config from the compile-time defaults. Safe to call
// more than once; later calls reset to defaults.
void rc_begin();

// Overlay a JSON document (the contents of /config/device.json) onto the
// effective config. Returns false and fills `err` when the document cannot be
// parsed, in which case the config is left untouched.
bool rc_apply_json(const char* json, char* err, size_t err_size);

// True once rc_apply_json() has successfully applied at least one override.
bool rc_has_overrides();

// True when the SSID came from the SD card rather than the build. Callers must
// not then apply build-time AP hints such as WIFI_BSSID/WIFI_CHANNEL, which
// describe a different network.
bool rc_wifi_overridden();

// Number of individual fields overridden by the last successful apply.
uint8_t rc_override_count();

const RuntimeConfig& rc();

// Accessors used at call sites. These return stable pointers into the config
// struct, so they are safe to hand to WiFi.begin(), snprintf(), etc.
const char* rc_room_name();
const char* rc_wifi_ssid();
const char* rc_wifi_pass();
const char* rc_mqtt_host();
uint16_t rc_mqtt_port();
const char* rc_mqtt_user();
const char* rc_mqtt_pass();
const char* rc_mqtt_pub_base();
const char* rc_mqtt_sub_base();
const char* rc_ota_password();
bool rc_ota_enabled();
uint32_t rc_sample_interval_sec();
bool rc_history_enabled();
bool rc_logs_enabled();
uint16_t rc_history_retention_days();

// Runtime override of the sampling cadence (MQTT command). Bounded by
// RC_MIN/MAX_SAMPLE_INTERVAL_SEC; out-of-range values are ignored.
bool rc_set_sample_interval_sec(uint32_t sec);

// Log the effective config, with secrets redacted.
void rc_log_summary();
