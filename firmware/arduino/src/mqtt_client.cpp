// MQTT client implementation - extracted from net.h
#include "mqtt_client.h"
#include <Preferences.h>
#include <strings.h>  // For strcasecmp
#include <cstdio>
#include "generated_config.h"
#include "config.h"
#include "runtime_config.h"
#include "metrics_diagnostics.h"
#include "debug_commands.h"
#include "profiling.h"
#include "safe_strings.h"
#include "wifi_manager.h"
#include "power.h"  // For BatteryStatus
#if LOG_MQTT_ENABLED
#include "logging/log_mqtt.h"
#endif

// External C function declarations (must be at file scope for extern "C")
#if USE_DISPLAY
extern "C" void display_capture_handle(const char* payload, size_t length);
#endif

// Static storage
static WiFiClient g_wifi_client;
static PubSubClient g_mqtt(g_wifi_client);
static OutsideReadings g_outside;
static char g_mqtt_client_id[40];  // Renamed to avoid conflict with net.h
static Preferences g_mqtt_prefs;

// Diagnostic mode flags
static bool g_diagnostic_mode_requested = false;
static bool g_diagnostic_mode_request_value = false;

// Rate limiting for MQTT commands (prevents flooding/DoS)
static uint32_t g_last_command_ms = 0;
static const uint32_t COMMAND_COOLDOWN_MS = 1000;  // 1 second between commands

// The outdoor alias topics are retained, and Home Assistant publishes the literal
// strings "unavailable"/"unknown" to them when the source entity loses its value
// (see homeassistant/mqtt_outdoor_publish.yaml, which publishes raw states()).
// Numeric handlers detect that through a failed strtof; text handlers have no such
// signal, so they check the payload against these sentinels explicitly. An empty
// payload (the retained-value clear) counts as "no value" too.
static bool is_no_value_payload(const char* s) {
  if (!s || s[0] == '\0')
    return true;
  return strcasecmp(s, "unavailable") == 0 || strcasecmp(s, "unknown") == 0 ||
         strcasecmp(s, "none") == 0;
}

// Helper to build MQTT topic (buffer-based to avoid heap fragmentation)
static void build_topic_buf(char* out, size_t out_size, const char* suffix) {
  if (out_size == 0)
    return;
  if (g_mqtt_client_id[0] == '\0') {
    // Client ID not set - use fallback
    safe_snprintf_rt(out, out_size, "espsensor/unknown/%s", suffix ? suffix : "");
    return;
  }
  safe_snprintf_rt(out, out_size, "espsensor/%s/%s", g_mqtt_client_id, suffix ? suffix : "");
}

// Deprecated - use build_topic_buf instead (kept for compatibility)
static String build_topic(const char* suffix) {
  char buf[96];
  build_topic_buf(buf, sizeof(buf), suffix);
  return String(buf);
}

void mqtt_begin() {
  // Client ID will be set externally via mqtt_set_client_id
  // to avoid WiFi dependency in this module

  // Configure MQTT client
  g_mqtt.setBufferSize(MQTT_MAX_PACKET_SIZE);

  // Server comes from the runtime config so an SD card override takes effect.
  if (rc_mqtt_host()[0] != '\0') {
    g_mqtt.setServer(rc_mqtt_host(), rc_mqtt_port());
  }

  // Set up MQTT callback for commands and outdoor data
  // NOTE: Using C strings (not Arduino String) to avoid heap fragmentation
  g_mqtt.setCallback([](char* topic, byte* payload, unsigned int length) {
    // Null check for safety
    if (!topic)
      return;

    // Helper to check if topic ends with suffix (C string version - no heap allocation)
    auto topic_ends_with = [](const char* t, const char* suffix) -> bool {
      size_t tlen = strlen(t);
      size_t slen = strlen(suffix);
      if (tlen < slen)
        return false;
      return strcmp(t + tlen - slen, suffix) == 0;
    };

    // Rate limit command topics (prevents flooding/DoS attacks)
    // Data topics (temp, weather) are not rate limited
    if (strstr(topic, "/cmd/") != nullptr) {
      uint32_t now = millis();
      if (now - g_last_command_ms < COMMAND_COOLDOWN_MS) {
        Serial.println("[MQTT] Command rate limited - ignoring");
        return;
      }
      g_last_command_ms = now;
    }

    // Handle diagnostic mode commands
    if (topic_ends_with(topic, "/cmd/diagnostic_mode")) {
      if (length > 0) {
        char value = static_cast<char>(payload[0]);
        g_diagnostic_mode_requested = true;
        g_diagnostic_mode_request_value = (value == '1' || value == 't' || value == 'T');
      }
      return;
    }

    // Handle sleep interval command (minimum 180s / 3 min to prevent sensor heating)
    if (topic_ends_with(topic, "/cmd/sleep_interval")) {
      if (length > 0 && length < 16) {
        char buf[16];
        memcpy(buf, payload, length);
        buf[length] = '\0';
        char* endptr = nullptr;
        long interval = strtol(buf, &endptr, 10);
        if (endptr != buf && interval >= 180 && interval <= 3600) {
          // Store new interval - will be used on next sleep cycle
          extern void set_custom_sleep_interval(uint32_t sec);
          set_custom_sleep_interval(static_cast<uint32_t>(interval));
          // The always-on scheduler reads rc_sample_interval_sec(), not the
          // deep-sleep custom interval above — without this the command printed
          // success and changed nothing on an always-on node.
          rc_set_sample_interval_sec(static_cast<uint32_t>(interval));
          Serial.printf("[MQTT] Sleep interval set to %ld seconds\n", interval);
        } else {
          Serial.println("[MQTT] Invalid sleep interval (must be 180-3600 seconds)");
        }
      }
      return;
    }

    // Handle device mode command (dev or production)
    if (topic_ends_with(topic, "/cmd/mode")) {
      if (length > 0 && length < 16) {
        char buf[16];
        memcpy(buf, payload, length);
        buf[length] = '\0';
        extern void set_device_mode(const char* mode);
        set_device_mode(buf);
      }
      return;
    }

    // Flip the display page (same as pressing BOOT). Payload ignored.
    if (topic_ends_with(topic, "/cmd/page")) {
      extern void request_page_flip();
      request_page_flip();
      return;
    }

    // Handle status request command
    if (topic_ends_with(topic, "/cmd/status")) {
      extern void publish_device_status();
      publish_device_status();
      return;
    }

    // Handle reboot command
    if (topic_ends_with(topic, "/cmd/reboot")) {
      Serial.println("[MQTT] Reboot command received");
      Serial.flush();
      delay(100);
      esp_restart();
      return;
    }

    // Handle screenshot command
    if (topic_ends_with(topic, "/cmd/screenshot")) {
#if USE_DISPLAY
      display_capture_handle((const char*)payload, length);
#endif
      return;
    }

// Forward log commands to LogMQTT
#if LOG_MQTT_ENABLED
    if (strstr(topic, "/cmd/clear_logs") != nullptr || strstr(topic, "/cmd/log_level") != nullptr ||
        strstr(topic, "/cmd/log_filter") != nullptr) {
      log_mqtt_handle_command(topic, (const uint8_t*)payload, length);
      return;
    }
#endif

    // Handle debug commands
    if (strstr(topic, "/cmd/debug") != nullptr) {
      debug_commands_handle(topic, (const uint8_t*)payload, length);
      return;
    }

// Handle outdoor weather data (alias topics)
#ifdef MQTT_SUB_BASE
    // Convert payload to string
    char value_str[64];
    if (length < sizeof(value_str)) {
      memcpy(value_str, payload, length);
      value_str[length] = '\0';
    } else {
      // Payload too large - truncate and log warning
      memcpy(value_str, payload, sizeof(value_str) - 1);
      value_str[sizeof(value_str) - 1] = '\0';
      Serial.printf("[MQTT] WARN: Payload truncated (%u bytes > %u max)\n", (unsigned)length,
                    (unsigned)(sizeof(value_str) - 1));
      increment_error_stat("mqtt_truncation");
    }

    // Handle temperature in Fahrenheit
    if (topic_ends_with(topic, "/temp_f")) {
      // Use strtof to properly detect parse errors (atof returns 0.0 for both error and valid "0")
      char* endptr = nullptr;
      float temp_f = strtof(value_str, &endptr);
      // Valid parse: endptr moved past start AND value is finite
      if (endptr != value_str && isfinite(temp_f)) {
        // Convert to Celsius and update outside readings
        float temp_c = (temp_f - 32.0f) * 5.0f / 9.0f;
        g_outside.temperatureC = temp_c;
        g_outside.validTemp = true;
      } else {
        // Same retained-topic hazard as pressure below: an unparseable payload
        // clears the reading rather than falling through, because leaving the last
        // good value with validTemp still set renders a stale temperature as if it
        // were current, indefinitely.
        g_outside.temperatureC = NAN;
        g_outside.validTemp = false;
      }
    } else if (topic_ends_with(topic, "/temp")) {
      // Handle temperature in Celsius (legacy)
      char* endptr = nullptr;
      float temp_c = strtof(value_str, &endptr);
      if (endptr != value_str && isfinite(temp_c)) {
        g_outside.temperatureC = temp_c;
        g_outside.validTemp = true;
      } else {
        g_outside.temperatureC = NAN;
        g_outside.validTemp = false;
      }
    } else if (topic_ends_with(topic, "/hum") || topic_ends_with(topic, "/rh")) {
      // Handle relative humidity in percent
      char* endptr = nullptr;
      float hum_pct = strtof(value_str, &endptr);
      if (endptr != value_str && isfinite(hum_pct)) {
        g_outside.humidityPct = hum_pct;
        g_outside.validHum = true;
      } else {
        g_outside.humidityPct = NAN;
        g_outside.validHum = false;
      }
    } else if (topic_ends_with(topic, "/wind_mps") || topic_ends_with(topic, "/wind")) {
      // Handle wind speed in metres per second (the renderer converts to mph)
      char* endptr = nullptr;
      float wind_mps = strtof(value_str, &endptr);
      if (endptr != value_str && isfinite(wind_mps)) {
        g_outside.windMps = wind_mps;
        g_outside.validWind = true;
      } else {
        g_outside.windMps = NAN;
        g_outside.validWind = false;
      }
    } else if (topic_ends_with(topic, "/pressure_hpa") || topic_ends_with(topic, "/pressure")) {
      // Handle barometric pressure in hPa ("/pressure" is the legacy alias; both
      // carry the same unit, so unlike temp_f/temp they need no conversion split).
      //
      // An unparseable payload clears validity instead of leaving the last good
      // reading in place: these topics are retained, and Home Assistant publishes
      // "unavailable"/"unknown" when the source entity loses its value. Keeping
      // the stale number would render it as current pressure indefinitely.
      char* endptr = nullptr;
      float pressure_hpa = strtof(value_str, &endptr);
      if (endptr != value_str && isfinite(pressure_hpa)) {
        g_outside.pressureHPa = pressure_hpa;
        g_outside.validPressure = true;
      } else {
        g_outside.pressureHPa = NAN;
        g_outside.validPressure = false;
      }
    } else if (topic_ends_with(topic, "/condition")) {
      // Handle weather condition text
      if (is_no_value_payload(value_str)) {
        // "unavailable" is not a weather condition: stored verbatim it would flow
        // through the icon mapping's default arm and draw a confident sunny icon.
        g_outside.weather[0] = '\0';
        g_outside.validWeather = false;
      } else {
        snprintf(g_outside.weather, sizeof(g_outside.weather), "%s", value_str);
        g_outside.validWeather = true;
      }
    } else if (topic_ends_with(topic, "/weather")) {
      // Handle weather description (legacy)
      if (is_no_value_payload(value_str)) {
        g_outside.weather[0] = '\0';
        g_outside.validWeather = false;
      } else {
        snprintf(g_outside.weather, sizeof(g_outside.weather), "%s", value_str);
        g_outside.validWeather = true;
      }
    } else if (topic_ends_with(topic, "/condition_code")) {
      // Handle weather condition code (currently not stored separately)
      // Weather code could be used to map to weather text if needed
      // For now, we rely on the condition text itself
    } else if (topic_ends_with(topic, "/weather_id")) {
      // Handle weather ID (legacy - currently not stored separately)
      // Weather ID could be used to map to weather text if needed
      // For now, we rely on the weather text itself
    }
#endif
  });
}

void mqtt_loop() {
  if (g_mqtt.connected()) {
    g_mqtt.loop();
  }
}

bool mqtt_connect() {
  PROFILE_SCOPE("mqtt_connect");
  // Assume caller has already checked WiFi connectivity
  // This removes WiFi dependency from mqtt module

  if (g_mqtt.connected()) {
    return true;
  }

  // Build LWT topic and message
  char lwt_topic[96];
  build_topic_buf(lwt_topic, sizeof(lwt_topic), "availability");

  // Connect with authentication if configured. An empty user means anonymous;
  // passing "" as the username makes some brokers reject the connection.
  bool connected = false;
  if (rc_mqtt_user()[0] != '\0') {
    connected = g_mqtt.connect(g_mqtt_client_id, rc_mqtt_user(), rc_mqtt_pass(), lwt_topic, 0, true,
                               "offline");
  } else {
    connected = g_mqtt.connect(g_mqtt_client_id, lwt_topic, 0, true, "offline");
  }

  if (connected) {
    // Publish online status
    g_mqtt.publish(lwt_topic, "online", true);

    // Publish device discovery message
    char discovery_topic[96];
    snprintf(discovery_topic, sizeof(discovery_topic), "espsensor/discovery/%s", g_mqtt_client_id);

    // Build discovery payload with device info
    char discovery_payload[256];
    // Unconditional: wifi_manager is always in the build, and the previous
    // FEATURE_WIFI guard referenced a flag that was never defined anywhere, so
    // this always fell through to a hardcoded 0.0.0.0.
    char ip_buf[16];
    wifi_get_ip_cstr(ip_buf, sizeof(ip_buf));

    snprintf(
        discovery_payload, sizeof(discovery_payload),
        "{\"device_id\":\"%s\",\"ip\":\"%s\",\"version\":\"%s\",\"room\":\"%s\",\"uptime\":%lu}",
        g_mqtt_client_id, ip_buf, FW_VERSION, rc_room_name(), millis() / 1000);

    // Publish discovery (not retained - will clear on disconnect via LWT)
    g_mqtt.publish(discovery_topic, discovery_payload, false);

    // Subscribe to command topics
    char cmd_topic[96];
    build_topic_buf(cmd_topic, sizeof(cmd_topic), "cmd/+");
    g_mqtt.subscribe(cmd_topic);

    // Initialize debug commands
    DebugCommands::getInstance().setClientId(g_mqtt_client_id);
    DebugCommands::getInstance().begin();

    // Subscribe to outdoor weather data (alias topics). Skipped when no base
    // topic is configured, which would otherwise subscribe to "/temp_f" and
    // friends at the broker root.
    if (rc_mqtt_sub_base()[0] != '\0') {
      String outdoor_base = String(rc_mqtt_sub_base());
      // Subscribe to alias topics for outdoor data
      String sub_topics[] = {
          outdoor_base + "/temp_f",          // Temperature in Fahrenheit
          outdoor_base + "/rh",              // Relative humidity in percent
          outdoor_base + "/wind_mps",        // Wind speed in metres per second
          outdoor_base + "/pressure_hpa",    // Barometric pressure in hPa
          outdoor_base + "/condition",       // Weather condition text
          outdoor_base + "/condition_code",  // Weather condition code
          // Legacy topics for backward compatibility
          outdoor_base + "/temp",       // Temperature in Celsius
          outdoor_base + "/hum",        // Relative humidity in percent
          outdoor_base + "/wind",       // Wind speed in metres per second
          outdoor_base + "/pressure",   // Barometric pressure in hPa
          outdoor_base + "/weather",    // Weather description
          outdoor_base + "/weather_id"  // Weather ID
      };

      for (const String& topic : sub_topics) {
        g_mqtt.subscribe(topic.c_str());
      }
    }
  }

  return connected;
}

bool mqtt_is_connected() { return g_mqtt.connected(); }

void mqtt_disconnect() {
  if (g_mqtt.connected()) {
    // Publish offline status before disconnecting
    char lwt_topic[96];
    build_topic_buf(lwt_topic, sizeof(lwt_topic), "availability");
    g_mqtt.publish(lwt_topic, "offline", true);
    g_mqtt.disconnect();
  }
}

void publish_device_status() {
  if (!g_mqtt.connected())
    return;

  // Get device mode and sleep interval
  extern const char* get_device_mode_str();
  extern uint32_t get_dev_mode_remaining_sec();
  extern uint32_t get_custom_sleep_interval();

  const char* mode = get_device_mode_str();
  uint32_t dev_timeout_sec = get_dev_mode_remaining_sec();
  uint32_t sleep_interval = get_custom_sleep_interval();
  if (sleep_interval == 0)
    sleep_interval = 600;  // Default 10 min

  // Get battery status (read_battery_status() declared in power.h)
  BatteryStatus bs = read_battery_status();

  // Build status JSON
  char topic_buf[96];
  build_topic_buf(topic_buf, sizeof(topic_buf), "status");

  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"mode\":\"%s\",\"sleep_interval_sec\":%lu,\"dev_mode_timeout_sec\":%lu,"
           "\"uptime_sec\":%lu,\"battery_pct\":%d,\"heap_free\":%lu,\"fw_version\":\"%s\",\"room\":"
           "\"%s\"}",
           mode, (unsigned long)sleep_interval, (unsigned long)dev_timeout_sec,
           (unsigned long)(millis() / 1000), bs.percent, (unsigned long)ESP.getFreeHeap(),
           FW_VERSION, rc_room_name());

  g_mqtt.publish(topic_buf, payload, false);
  Serial.printf("[MQTT] Published device status: mode=%s, interval=%lus\n", mode,
                (unsigned long)sleep_interval);
}

void mqtt_set_client_id(const char* client_id) {
  if (client_id) {
    snprintf(g_mqtt_client_id, sizeof(g_mqtt_client_id), "%s", client_id);
  }
}

const char* mqtt_get_client_id() { return g_mqtt_client_id; }

void mqtt_set_server(const char* server, uint16_t port) { g_mqtt.setServer(server, port); }

bool mqtt_publish_raw(const char* topic, const char* payload, bool retain) {
  if (!g_mqtt.connected())
    return false;
  return g_mqtt.publish(topic, payload, retain);
}

// Publishing implementations
void mqtt_publish_inside(float tempC, float rhPct) {
  if (!g_mqtt.connected())
    return;

  char topic_buf[96];
  char payload[64];

  // Publish temperature
  if (isfinite(tempC)) {
    snprintf(payload, sizeof(payload), "%.1f", tempC);
    build_topic_buf(topic_buf, sizeof(topic_buf), "inside/temperature");
    g_mqtt.publish(topic_buf, payload, true);
  }

  // Publish humidity
  if (isfinite(rhPct)) {
    snprintf(payload, sizeof(payload), "%.1f", rhPct);
    build_topic_buf(topic_buf, sizeof(topic_buf), "inside/humidity");
    g_mqtt.publish(topic_buf, payload, true);
  }
}

void mqtt_publish_pressure(float pressureHPa) {
  if (!g_mqtt.connected() || !isfinite(pressureHPa))
    return;

  char topic_buf[96];
  char payload[32];
  snprintf(payload, sizeof(payload), "%.1f", pressureHPa);
  build_topic_buf(topic_buf, sizeof(topic_buf), "inside/pressure");
  g_mqtt.publish(topic_buf, payload, true);
}

void mqtt_publish_battery(float voltage, int percent) {
  if (!g_mqtt.connected())
    return;

  char topic_buf[96];
  char payload[32];

  // Publish voltage
  if (isfinite(voltage)) {
    snprintf(payload, sizeof(payload), "%.2f", voltage);
    build_topic_buf(topic_buf, sizeof(topic_buf), "battery/voltage");
    g_mqtt.publish(topic_buf, payload, true);
  }

  // Publish percentage
  if (percent >= 0) {
    snprintf(payload, sizeof(payload), "%d", percent);
    build_topic_buf(topic_buf, sizeof(topic_buf), "battery/percent");
    g_mqtt.publish(topic_buf, payload, true);
  }
}

void mqtt_publish_wifi_rssi(int rssiDbm) {
  if (!g_mqtt.connected())
    return;

  char topic_buf[96];
  char payload[16];
  snprintf(payload, sizeof(payload), "%d", rssiDbm);
  build_topic_buf(topic_buf, sizeof(topic_buf), "wifi/rssi");
  g_mqtt.publish(topic_buf, payload, true);
}

void mqtt_publish_status(const char* payload, bool retain) {
  if (!g_mqtt.connected() || !payload)
    return;

  char topic_buf[96];
  build_topic_buf(topic_buf, sizeof(topic_buf), "status");
  g_mqtt.publish(topic_buf, payload, retain);
}

void mqtt_publish_debug_json(const char* payload, bool retain) {
  if (!g_mqtt.connected() || !payload)
    return;

  char topic_buf[96];
  build_topic_buf(topic_buf, sizeof(topic_buf), "debug/json");
  g_mqtt.publish(topic_buf, payload, retain);
}

void mqtt_publish_last_crash(const char* reason_or_null) {
  if (!g_mqtt.connected())
    return;

  char topic_buf[96];
  build_topic_buf(topic_buf, sizeof(topic_buf), "debug/last_crash");
  if (reason_or_null) {
    g_mqtt.publish(topic_buf, reason_or_null, true);
  } else {
    g_mqtt.publish(topic_buf, "none", true);
  }
}

void mqtt_publish_debug_probe(const char* payload, bool retain) {
  if (!g_mqtt.connected() || !payload)
    return;

  char topic_buf[96];
  build_topic_buf(topic_buf, sizeof(topic_buf), "debug/probe");
  g_mqtt.publish(topic_buf, payload, retain);
}

void mqtt_publish_boot_reason(const char* reason) {
  if (!g_mqtt.connected() || !reason)
    return;

  char topic_buf[96];
  build_topic_buf(topic_buf, sizeof(topic_buf), "debug/boot_reason");
  g_mqtt.publish(topic_buf, reason, true);
}

void mqtt_publish_boot_count(uint32_t count) {
  if (!g_mqtt.connected())
    return;

  char topic_buf[96];
  char payload[16];
  snprintf(payload, sizeof(payload), "%u", count);
  build_topic_buf(topic_buf, sizeof(topic_buf), "debug/boot_count");
  g_mqtt.publish(topic_buf, payload, true);
}

void mqtt_publish_crash_count(uint32_t count) {
  if (!g_mqtt.connected())
    return;

  char topic_buf[96];
  char payload[16];
  snprintf(payload, sizeof(payload), "%u", count);
  build_topic_buf(topic_buf, sizeof(topic_buf), "debug/crash_count");
  g_mqtt.publish(topic_buf, payload, true);
}

void mqtt_publish_uptime(uint32_t uptime_sec) {
  if (!g_mqtt.connected())
    return;

  char topic_buf[96];
  char payload[16];
  snprintf(payload, sizeof(payload), "%u", uptime_sec);
  build_topic_buf(topic_buf, sizeof(topic_buf), "debug/uptime");
  g_mqtt.publish(topic_buf, payload, true);
}

void mqtt_publish_wake_count(uint32_t count) {
  if (!g_mqtt.connected())
    return;

  char topic_buf[96];
  char payload[16];
  snprintf(payload, sizeof(payload), "%u", count);
  build_topic_buf(topic_buf, sizeof(topic_buf), "debug/wake_count");
  g_mqtt.publish(topic_buf, payload, true);
}

void mqtt_publish_memory_diagnostics(uint32_t free_heap, uint32_t min_heap, uint32_t largest_block,
                                     float fragmentation_pct) {
  if (!g_mqtt.connected())
    return;

  char topic_buf[96];
  char payload[128];
  snprintf(payload, sizeof(payload), "{\"free\":%u,\"min\":%u,\"largest\":%u,\"frag\":%.1f}",
           free_heap, min_heap, largest_block, fragmentation_pct);
  build_topic_buf(topic_buf, sizeof(topic_buf), "debug/memory");
  g_mqtt.publish(topic_buf, payload, true);
}

void mqtt_publish_diagnostic_mode(bool active) {
  if (!g_mqtt.connected())
    return;

  char topic_buf[96];
  build_topic_buf(topic_buf, sizeof(topic_buf), "diagnostic_mode");
  g_mqtt.publish(topic_buf, active ? "true" : "false", true);
}

void mqtt_publish_publish_latency_ms(uint32_t publishLatencyMs) {
  if (!g_mqtt.connected())
    return;

  char topic_buf[96];
  char payload[16];
  snprintf(payload, sizeof(payload), "%u", publishLatencyMs);
  build_topic_buf(topic_buf, sizeof(topic_buf), "debug/publish_latency_ms");
  g_mqtt.publish(topic_buf, payload, true);
}

// Outside readings management

OutsideReadings mqtt_get_outside_readings() { return g_outside; }

// Diagnostic mode
bool mqtt_is_diagnostic_mode_requested() { return g_diagnostic_mode_requested; }

bool mqtt_get_diagnostic_mode_value() { return g_diagnostic_mode_request_value; }

void mqtt_clear_diagnostic_mode_request() { g_diagnostic_mode_requested = false; }

PubSubClient* mqtt_get_client() { return &g_mqtt; }
