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

static RuntimeConfig g_rc;
static uint8_t g_override_count = 0;

void rc_begin() {
  memset(&g_rc, 0, sizeof(g_rc));
  g_override_count = 0;

  // Compile-time defaults. merge_str() is used rather than strncpy so the
  // truncation and NUL-termination rules stay in exactly one place.
  merge_str(g_rc.room_name, sizeof(g_rc.room_name), ROOM_NAME);
  merge_str(g_rc.wifi_ssid, sizeof(g_rc.wifi_ssid), WIFI_SSID);
  merge_str(g_rc.wifi_pass, sizeof(g_rc.wifi_pass), WIFI_PASS);
  merge_str(g_rc.mqtt_host, sizeof(g_rc.mqtt_host), MQTT_HOST);
  merge_str(g_rc.mqtt_user, sizeof(g_rc.mqtt_user), MQTT_USER);
  merge_str(g_rc.mqtt_pass, sizeof(g_rc.mqtt_pass), MQTT_PASS);
  merge_str(g_rc.mqtt_pub_base, sizeof(g_rc.mqtt_pub_base), MQTT_PUB_BASE);
  merge_str(g_rc.mqtt_sub_base, sizeof(g_rc.mqtt_sub_base), MQTT_SUB_BASE);
  g_rc.mqtt_port = MQTT_PORT;

#ifdef OTA_PASSWORD
  merge_str(g_rc.ota_password, sizeof(g_rc.ota_password), OTA_PASSWORD);
#endif
  g_rc.ota_enabled = (FEATURE_OTA != 0);

  g_rc.sample_interval_sec = SAMPLE_INTERVAL_SEC;
  g_rc.history_enabled = true;
  g_rc.logs_enabled = true;
  g_rc.history_retention_days = SD_HISTORY_RETENTION_DAYS;
}

bool rc_apply_json(const char* json, char* err, size_t err_size) {
  auto fail = [&](const char* msg) {
    if (err && err_size > 0) {
      strncpy(err, msg, err_size - 1);
      err[err_size - 1] = '\0';
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
  applied += merge_str(g_rc.room_name, sizeof(g_rc.room_name), doc["room_name"] | "");
  applied +=
      merge_u32(&g_rc.sample_interval_sec, doc["sample_interval_sec"] | static_cast<int64_t>(-1),
                RC_MIN_SAMPLE_INTERVAL_SEC, RC_MAX_SAMPLE_INTERVAL_SEC);

  JsonObject wifi = doc["wifi"];
  if (!wifi.isNull()) {
    applied += merge_str(g_rc.wifi_ssid, sizeof(g_rc.wifi_ssid), wifi["ssid"] | "");
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
    applied += merge_bool(&g_rc.ota_enabled, !ota["enabled"].isNull(), ota["enabled"] | false);
    applied += merge_str(g_rc.ota_password, sizeof(g_rc.ota_password), ota["password"] | "");
  }

  JsonObject storage = doc["storage"];
  if (!storage.isNull()) {
    applied += merge_bool(&g_rc.history_enabled, !storage["history_enabled"].isNull(),
                          storage["history_enabled"] | true);
    applied += merge_bool(&g_rc.logs_enabled, !storage["logs_enabled"].isNull(),
                          storage["logs_enabled"] | true);
    applied += merge_u16(&g_rc.history_retention_days,
                         storage["history_retention_days"] | static_cast<int64_t>(-1), 0, 3650);
  }

  g_override_count = applied;
  return true;
}

bool rc_has_overrides() { return g_override_count > 0; }

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
  LOG_INFO("SD history: %s, SD logs: %s", g_rc.history_enabled ? "on" : "off",
           g_rc.logs_enabled ? "on" : "off");
  LOG_INFO("Overrides from SD: %u field(s)", g_override_count);
}
