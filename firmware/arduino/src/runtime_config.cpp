// Runtime configuration implementation
// Copyright 2024 Justin

#include "runtime_config.h"
#include <ArduinoJson.h>
#include "config.h"
#include "config_merge.h"
#include "credential_safe.h"
#include "feature_flags.h"
#include "generated_config.h"
#include "logging.h"
#include "safe_strings.h"

static RuntimeConfig g_rc;
static uint8_t g_override_count = 0;
static bool g_wifi_overridden = false;

// Replace characters that cannot appear raw inside a JSON string literal. Done
// in place; the room name is display text, so substitution is preferable to
// rejecting the value or growing the buffer with escapes.
static void sanitize_for_json(char* s) {
  if (!s)
    return;
  for (char* c = s; *c; ++c) {
    unsigned char u = static_cast<unsigned char>(*c);
    if (*c == '"' || *c == '\\' || u < 0x20 || u == 0x7F) {
      *c = ' ';
    }
  }
}

void rc_begin() {
  memset(&g_rc, 0, sizeof(g_rc));
  g_override_count = 0;
  g_wifi_overridden = false;

  // Compile-time defaults. merge_str() is used rather than strncpy so the
  // truncation and NUL-termination rules stay in exactly one place.
  merge_str(g_rc.room_name, sizeof(g_rc.room_name), ROOM_NAME);
  // Consumers treat rc_room_name() as safe to drop straight into hand-built JSON
  // (ha_discovery, mqtt_client). That invariant has to hold for the compile-time
  // value too: a room_name in device.yaml containing a quote reaches ROOM_NAME
  // unescaped, and only the JSON-overlay path was sanitising.
  sanitize_for_json(g_rc.room_name);
  merge_str(g_rc.wifi_ssid, sizeof(g_rc.wifi_ssid), WIFI_SSID);
  merge_str(g_rc.wifi_pass, sizeof(g_rc.wifi_pass), WIFI_PASS);
  merge_str(g_rc.mqtt_host, sizeof(g_rc.mqtt_host), MQTT_HOST);
  merge_str(g_rc.mqtt_user, sizeof(g_rc.mqtt_user), MQTT_USER);
  merge_str(g_rc.mqtt_pass, sizeof(g_rc.mqtt_pass), MQTT_PASS);
  merge_str(g_rc.mqtt_pub_base, sizeof(g_rc.mqtt_pub_base), MQTT_PUB_BASE);
  merge_str(g_rc.mqtt_sub_base, sizeof(g_rc.mqtt_sub_base), MQTT_SUB_BASE);
  // Compile-time numerics get the same bounds as the JSON path. Assigning
  // MQTT_PORT straight through would let a bad -D flag wrap the uint16_t
  // narrowing conversion instead of falling back to something usable.
  g_rc.mqtt_port = 1883;
  if (!merge_u16(&g_rc.mqtt_port, MQTT_PORT, 1, 65535)) {
    LOG_WARN("Config: MQTT_PORT=%d out of range [1,65535], using 1883", MQTT_PORT);
  }

#ifdef OTA_PASSWORD
  merge_str(g_rc.ota_password, sizeof(g_rc.ota_password), OTA_PASSWORD);
#endif
  g_rc.ota_enabled = (FEATURE_OTA != 0);

  // Start from a known-good value, then accept the compile-time one only if it
  // is in range. A build flag or device.yaml entry of 0 would otherwise make the
  // always-on loop sample every iteration and cook the temperature reading; the
  // JSON path already validates, this closes the same hole for the other two
  // sources.
  g_rc.sample_interval_sec = RC_DEFAULT_SAMPLE_INTERVAL_SEC;
  if (!merge_u32(&g_rc.sample_interval_sec, SAMPLE_INTERVAL_SEC, RC_MIN_SAMPLE_INTERVAL_SEC,
                 RC_MAX_SAMPLE_INTERVAL_SEC)) {
    LOG_WARN("Config: SAMPLE_INTERVAL_SEC=%d out of range [%d,%d], using %d", SAMPLE_INTERVAL_SEC,
             RC_MIN_SAMPLE_INTERVAL_SEC, RC_MAX_SAMPLE_INTERVAL_SEC,
             RC_DEFAULT_SAMPLE_INTERVAL_SEC);
  }
  g_rc.history_enabled = true;
  g_rc.logs_enabled = true;
  g_rc.history_retention_days = 90;
  if (!merge_u16(&g_rc.history_retention_days, SD_HISTORY_RETENTION_DAYS, 0, 3650)) {
    LOG_WARN("Config: SD_HISTORY_RETENTION_DAYS=%d out of range [0,3650], using 90",
             SD_HISTORY_RETENTION_DAYS);
  }

  g_rc.temp_offset_c = 0.0f;
  if (!merge_float(&g_rc.temp_offset_c, true, TEMP_OFFSET_C, RC_MIN_TEMP_OFFSET_C,
                   RC_MAX_TEMP_OFFSET_C)) {
    LOG_WARN("Config: TEMP_OFFSET_C out of range [%d,%d], using 0",
             static_cast<int>(RC_MIN_TEMP_OFFSET_C), static_cast<int>(RC_MAX_TEMP_OFFSET_C));
  }
}

bool rc_apply_json(const char* json, char* err, size_t err_size) {
  auto fail = [&](const char* msg) {
    if (err && err_size > 0) {
      safe_strcpy_rt(err, err_size, msg);
    }
    return false;
  };

  if (!json || json[0] == '\0')
    return fail("empty document");

  JsonDocument doc;
  DeserializationError parse_err = deserializeJson(doc, json);
  if (parse_err)
    return fail(parse_err.c_str());
  if (!doc.is<JsonObject>())
    return fail("root is not an object");

  uint8_t applied = 0;

  // `| <default>` yields the default when the key is absent or the value has the
  // wrong type. -1 is used as the "absent" sentinel for numbers because every
  // valid range below starts above zero. The int64_t cast keeps an absurdly
  // large value in the file from wrapping during conversion.
  if (merge_str(g_rc.room_name, sizeof(g_rc.room_name), doc["room_name"] | "")) {
    // The room name is interpolated into hand-built JSON payloads (MQTT
    // discovery, Home Assistant discovery). A quote or backslash from the card
    // would produce malformed JSON, so neutralise them here, once, rather than
    // escaping at every consumer.
    sanitize_for_json(g_rc.room_name);
    applied++;
  }
  applied +=
      merge_u32(&g_rc.sample_interval_sec, doc["sample_interval_sec"] | static_cast<int64_t>(-1),
                RC_MIN_SAMPLE_INTERVAL_SEC, RC_MAX_SAMPLE_INTERVAL_SEC);

  // Floats have no workable sentinel (a negative offset is a legitimate value),
  // so presence comes from the key's type instead.
  applied += merge_float(&g_rc.temp_offset_c, doc["temp_offset_c"].is<float>(),
                         doc["temp_offset_c"] | 0.0, RC_MIN_TEMP_OFFSET_C, RC_MAX_TEMP_OFFSET_C);

  JsonObject wifi = doc["wifi"];
  if (!wifi.isNull()) {
    bool ssid_overridden = merge_str(g_rc.wifi_ssid, sizeof(g_rc.wifi_ssid), wifi["ssid"] | "");
    if (ssid_overridden) {
      g_wifi_overridden = true;
    }
    applied += ssid_overridden;
    applied += merge_str(g_rc.wifi_pass, sizeof(g_rc.wifi_pass), wifi["password"] | "");
  }

  JsonObject mqtt = doc["mqtt"];
  if (!mqtt.isNull()) {
    applied += merge_str(g_rc.mqtt_host, sizeof(g_rc.mqtt_host), mqtt["host"] | "");
    applied += merge_u16(&g_rc.mqtt_port, mqtt["port"] | static_cast<int64_t>(-1), 1, 65535);
    applied += merge_str(g_rc.mqtt_user, sizeof(g_rc.mqtt_user), mqtt["user"] | "");
    applied += merge_str(g_rc.mqtt_pass, sizeof(g_rc.mqtt_pass), mqtt["password"] | "");
    applied += merge_str(g_rc.mqtt_pub_base, sizeof(g_rc.mqtt_pub_base), mqtt["publish_base"] | "");
    applied +=
        merge_str(g_rc.mqtt_sub_base, sizeof(g_rc.mqtt_sub_base), mqtt["subscribe_base"] | "");
  }

  JsonObject ota = doc["ota"];
  if (!ota.isNull()) {
    applied += merge_bool(&g_rc.ota_enabled, ota["enabled"].is<bool>(), ota["enabled"] | false);
    applied += merge_str(g_rc.ota_password, sizeof(g_rc.ota_password), ota["password"] | "");
  }

  JsonObject storage = doc["storage"];
  if (!storage.isNull()) {
    applied += merge_bool(&g_rc.history_enabled, storage["history_enabled"].is<bool>(),
                          storage["history_enabled"] | true);
    applied += merge_bool(&g_rc.logs_enabled, storage["logs_enabled"].is<bool>(),
                          storage["logs_enabled"] | true);
    applied += merge_u16(&g_rc.history_retention_days,
                         storage["history_retention_days"] | static_cast<int64_t>(-1), 0, 3650);
  }

  g_override_count = applied;
  return true;
}

bool rc_has_overrides() { return g_override_count > 0; }

bool rc_wifi_overridden() { return g_wifi_overridden; }

uint8_t rc_override_count() { return g_override_count; }

const RuntimeConfig& rc() { return g_rc; }

const char* rc_room_name() { return g_rc.room_name; }
const char* rc_wifi_ssid() { return g_rc.wifi_ssid; }
const char* rc_wifi_pass() { return g_rc.wifi_pass; }
const char* rc_mqtt_host() { return g_rc.mqtt_host; }
uint16_t rc_mqtt_port() { return g_rc.mqtt_port; }
const char* rc_mqtt_user() { return g_rc.mqtt_user; }
const char* rc_mqtt_pass() { return g_rc.mqtt_pass; }
const char* rc_mqtt_pub_base() { return g_rc.mqtt_pub_base; }
const char* rc_mqtt_sub_base() { return g_rc.mqtt_sub_base; }
const char* rc_ota_password() { return g_rc.ota_password; }
bool rc_ota_enabled() { return g_rc.ota_enabled; }
uint32_t rc_sample_interval_sec() { return g_rc.sample_interval_sec; }
bool rc_history_enabled() { return g_rc.history_enabled; }
bool rc_logs_enabled() { return g_rc.logs_enabled; }
uint16_t rc_history_retention_days() { return g_rc.history_retention_days; }
float rc_temp_offset_c() { return g_rc.temp_offset_c; }

bool rc_set_sample_interval_sec(uint32_t sec) {
  return merge_u32(&g_rc.sample_interval_sec, sec, RC_MIN_SAMPLE_INTERVAL_SEC,
                   RC_MAX_SAMPLE_INTERVAL_SEC);
}

void rc_log_summary() {
  LOG_INFO("Room: %s", g_rc.room_name);
  LOG_INFO("WiFi SSID: %s", sanitize_ssid(g_rc.wifi_ssid).c_str());
  LOG_INFO("MQTT: %s:%u user=%s", g_rc.mqtt_host, g_rc.mqtt_port,
           sanitize_credential(g_rc.mqtt_user).c_str());
  LOG_INFO("Sample interval: %u s", g_rc.sample_interval_sec);
  LOG_INFO("OTA: %s (password %s)", g_rc.ota_enabled ? "enabled" : "disabled",
           g_rc.ota_password[0] ? "set" : "NOT SET");
  LOG_INFO("Temp offset: %.1f C", static_cast<double>(g_rc.temp_offset_c));
  LOG_INFO("SD history: %s, SD logs: %s", g_rc.history_enabled ? "on" : "off",
           g_rc.logs_enabled ? "on" : "off");
  LOG_INFO("Overrides from SD: %u field(s)", g_override_count);
}
