#include "debug_commands.h"
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <cstdio>
#include "mqtt_client.h"
#include "safe_strings.h"
#include "logging/logger.h"
#include "profiling.h"
#include "crash_handler.h"
#include "memory_tracking.h"
#include "power.h"
#include "feature_flags.h"
#include "mqtt_batcher.h"
#include "display_smart_refresh.h"
#include "sensors.h"
#include "config.h"
#if USE_DISPLAY
#include "display_capture.h"
#endif

// Serialize a sensor reading as a JSON number, or null when it has no value.
// Sensor floats default to NAN until the first reading lands, and "%.1f" prints
// those as "nan", which is not valid JSON - a consumer cannot even parse the
// document to discover the value is missing.
static void json_number(char* out, size_t out_size, float value, int decimals) {
  if (!isfinite(value)) {
    snprintf(out, out_size, "null");
    return;
  }
  snprintf(out, out_size, "%.*f", decimals, value);
}

// Strip BOTH braces of an inner JSON object so it can be merged into a wrapper.
// Stripping only the opening one -- as cmdMemory and cmdMqttBatch each once did
// independently -- keeps the inner closing brace, and the wrapper then appends
// another: the published payload ends in `}}` and no consumer can parse it.
// Mutates `json` (writes a NUL over the closing brace); returns the body.
static const char* json_object_body(char* json) {
  if (json[0] != '{')
    return json;
  size_t len = strlen(json);
  if (len >= 2 && json[len - 1] == '}')
    json[len - 1] = '\0';
  return json + 1;
}

DebugCommands& DebugCommands::getInstance() {
  static DebugCommands instance;
  return instance;
}

void DebugCommands::begin() {
  if (initialized_)
    return;

  // Subscribe to debug command topic using static buffer instead of String
  PubSubClient* client = mqtt_get_client();
  if (client && client->connected() && client_id_[0] != '\0') {
    char debug_topic[96];
    snprintf(debug_topic, sizeof(debug_topic), "espsensor/%s%s", client_id_, TOPIC_CMD_DEBUG);
    client->subscribe(debug_topic);
  }

  initialized_ = true;
}

void DebugCommands::setClientId(const char* client_id) {
  if (client_id) {
    safe_strcpy(client_id_, client_id);
  }
}

void DebugCommands::handleCommand(const char* topic, const uint8_t* payload, size_t length) {
  if (!initialized_)
    return;

  PubSubClient* client = mqtt_get_client();
  if (!client || !client->connected())
    return;

  // Parse JSON command
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    // Invalid JSON
    char response[128];
    snprintf(response, sizeof(response), "{\"error\":\"Invalid JSON\",\"code\":\"%s\"}",
             error.c_str());
    publishResponse(client, response);
    return;
  }

  const char* cmd = doc["cmd"];
  if (!cmd) {
    publishResponse(client, "{\"error\":\"Missing 'cmd' field\"}");
    return;
  }

  // Dispatch to appropriate handler
  if (strcmp(cmd, "heap") == 0) {
    cmdHeap(client);
  } else if (strcmp(cmd, "state") == 0) {
    cmdState(client);
  } else if (strcmp(cmd, "config") == 0) {
    cmdConfig(client);
  } else if (strcmp(cmd, "restart") == 0) {
    cmdRestart(client);
  } else if (strcmp(cmd, "modules") == 0) {
    cmdModules(client);
  } else if (strcmp(cmd, "uptime") == 0) {
    cmdUptime(client);
  } else if (strcmp(cmd, "network") == 0) {
    cmdNetwork(client);
  } else if (strcmp(cmd, "sensors") == 0) {
    cmdSensors(client);
  } else if (strcmp(cmd, "perf") == 0) {
    cmdPerf(client);
  } else if (strcmp(cmd, "perf_reset") == 0) {
    cmdPerfReset(client);
  } else if (strcmp(cmd, "crash") == 0) {
    cmdCrash(client);
  } else if (strcmp(cmd, "crash_clear") == 0) {
    cmdCrashClear(client);
  } else if (strcmp(cmd, "memory") == 0) {
    cmdMemory(client);
  } else if (strcmp(cmd, "memory_reset") == 0) {
    cmdMemoryReset(client);
  } else if (strcmp(cmd, "sleep") == 0) {
    cmdSleep(client);
  } else if (strcmp(cmd, "features") == 0) {
    cmdFeatures(client);
  } else if (strcmp(cmd, "mqtt_batch") == 0) {
    cmdMqttBatch(client);
  } else if (strcmp(cmd, "smart_refresh") == 0) {
    cmdSmartRefresh(client);
  } else if (strcmp(cmd, "screenshot") == 0) {
    cmdScreenshot(client);
  } else {
    char response[128];
    snprintf(response, sizeof(response), "{\"error\":\"Unknown command '%s'\"}", cmd);
    publishResponse(client, response);
  }
}

void DebugCommands::cmdHeap(PubSubClient* client) {
  uint32_t free_heap = esp_get_free_heap_size();
  uint32_t min_heap = esp_get_minimum_free_heap_size();
  uint32_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

  // Calculate fragmentation percentage
  float fragmentation = 0.0f;
  if (free_heap > 0) {
    fragmentation =
        100.0f * (1.0f - static_cast<float>(largest_block) / static_cast<float>(free_heap));
  }

  char response[256];
  snprintf(response, sizeof(response),
           "{\"cmd\":\"heap\",\"free\":%u,\"min\":%u,\"largest\":%u,\"frag\":%.1f}", free_heap,
           min_heap, largest_block, fragmentation);

  publishResponse(client, response);
}

void DebugCommands::cmdState(PubSubClient* client) {
  // Get outside readings from MQTT
  OutsideReadings outside = mqtt_get_outside_readings();

  char temp_c[16], humidity[16], pressure_hpa[16];
  json_number(temp_c, sizeof(temp_c), outside.temperatureC, 1);
  json_number(humidity, sizeof(humidity), outside.humidityPct, 0);
  json_number(pressure_hpa, sizeof(pressure_hpa), outside.pressureHPa, 1);

  char response[384];
  snprintf(response, sizeof(response),
           "{\"cmd\":\"state\",\"outside\":{\"temp_c\":%s,\"humidity\":%s,\"pressure_hpa\":%s,"
           "\"valid_temp\":%s,\"valid_humidity\":%s,\"valid_pressure\":%s}}",
           temp_c, humidity, pressure_hpa, outside.validTemp ? "true" : "false",
           outside.validHum ? "true" : "false", outside.validPressure ? "true" : "false");

  publishResponse(client, response);
}

void DebugCommands::cmdConfig(PubSubClient* client) {
  // Get logging configuration
  char log_config[256];
  Logger::getInstance().getConfigJson(log_config, sizeof(log_config));

  char response[384];
  snprintf(response, sizeof(response), "{\"cmd\":\"config\",\"logging\":%s}", log_config);

  publishResponse(client, response);
}

void DebugCommands::cmdRestart(PubSubClient* client) {
  publishResponse(client, "{\"cmd\":\"restart\",\"status\":\"restarting\"}");

  // Allow time for message to be sent
  delay(100);

  // Perform soft restart
  esp_restart();
}

void DebugCommands::cmdModules(PubSubClient* client) {
  Logger& logger = Logger::getInstance();
  uint8_t count = logger.getModuleCount();

  char response[512];
  int pos = snprintf(response, sizeof(response), "{\"cmd\":\"modules\",\"count\":%u,\"modules\":[",
                     count);

  for (uint8_t i = 0; i < count && pos < static_cast<int>(sizeof(response) - 64); i++) {
    if (i > 0) {
      pos += snprintf(response + pos, sizeof(response) - pos, ",");
    }

    const char* name = logger.getModuleName(i);
    bool enabled = logger.isModuleEnabled(i);

    // Bounds check before writing - reserve space for closing brackets
    if (pos >= static_cast<int>(sizeof(response) - 64)) {
      break;  // Stop if we're running out of space
    }

    pos += snprintf(response + pos, sizeof(response) - pos,
                    "{\"id\":%u,\"name\":\"%s\",\"enabled\":%s}", i, name ? name : "null",
                    enabled ? "true" : "false");
  }

  // Ensure we have space for closing brackets
  if (pos < static_cast<int>(sizeof(response) - 3)) {
    snprintf(response + pos, sizeof(response) - pos, "]}");
  } else {
    // Truncate and close - find safe spot
    response[sizeof(response) - 3] = ']';
    response[sizeof(response) - 2] = '}';
    response[sizeof(response) - 1] = '\0';
  }
  publishResponse(client, response);
}

void DebugCommands::cmdUptime(PubSubClient* client) {
  uint32_t uptime_ms = millis();
  uint32_t uptime_sec = uptime_ms / 1000;

  char response[128];
  snprintf(response, sizeof(response), "{\"cmd\":\"uptime\",\"ms\":%u,\"sec\":%u}", uptime_ms,
           uptime_sec);

  publishResponse(client, response);
}

void DebugCommands::cmdNetwork(PubSubClient* client) {
  bool wifi_connected = (WiFi.status() == WL_CONNECTED);
  int rssi = wifi_connected ? WiFi.RSSI() : 0;
  bool mqtt_connected = mqtt_is_connected();

  char response[256];
  snprintf(
      response, sizeof(response),
      "{\"cmd\":\"network\",\"wifi\":{\"connected\":%s,\"rssi\":%d},\"mqtt\":{\"connected\":%s}}",
      wifi_connected ? "true" : "false", rssi, mqtt_connected ? "true" : "false");

  publishResponse(client, response);
}

void DebugCommands::cmdSensors(PubSubClient* client) {
  // Read current sensor values
  InsideReadings readings = read_inside_sensors();

  char temp_c[16], temp_f[16], humidity[16], pressure[16];
  json_number(temp_c, sizeof(temp_c), readings.temperatureC, 1);
  json_number(temp_f, sizeof(temp_f), readings.temperatureC * 9.0f / 5.0f + 32.0f, 1);
  json_number(humidity, sizeof(humidity), readings.humidityPct, 0);
  json_number(pressure, sizeof(pressure), readings.pressureHPa, 1);

  char response[256];
  snprintf(response, sizeof(response),
           "{\"cmd\":\"sensors\","
           "\"temp_c\":%s,"
           "\"temp_f\":%s,"
           "\"humidity\":%s,"
           "\"pressure\":%s,"
           "\"valid\":%s}",
           temp_c, temp_f, humidity, pressure, isfinite(readings.temperatureC) ? "true" : "false");

  publishResponse(client, response);
}

void DebugCommands::cmdPerf(PubSubClient* client) {
  char response[1024];
  PerformanceMonitor::getInstance().formatJson(response, sizeof(response));
  publishResponse(client, response);
}

void DebugCommands::cmdPerfReset(PubSubClient* client) {
  PerformanceMonitor::getInstance().reset();
  publishResponse(client, "{\"cmd\":\"perf_reset\",\"status\":\"ok\"}");
}

void DebugCommands::cmdCrash(PubSubClient* client) {
  char report[512];
  CrashHandler::getInstance().formatCrashReport(report, sizeof(report));
  publishResponse(client, report);
}

void DebugCommands::cmdCrashClear(PubSubClient* client) {
  CrashHandler::getInstance().clearCrashInfo();
  publishResponse(client, "{\"cmd\":\"crash_clear\",\"status\":\"ok\"}");
}

void DebugCommands::cmdMemory(PubSubClient* client) {
  // Update tracking before reading
  MemoryTracker::getInstance().update();

  char stats[384];
  MemoryTracker::getInstance().formatStatsJson(stats, sizeof(stats));

  char response[512];
  snprintf(response, sizeof(response), "{\"cmd\":\"memory\",%s}", json_object_body(stats));
  publishResponse(client, response);
}

void DebugCommands::cmdMemoryReset(PubSubClient* client) {
  MemoryTracker::getInstance().resetAll();
  publishResponse(client, "{\"cmd\":\"memory_reset\",\"status\":\"ok\"}");
}

void DebugCommands::cmdSleep(PubSubClient* client) {
  SleepConfig config = get_default_sleep_config();
  uint32_t optimal = calculate_optimal_sleep_interval(config);
  BatteryStatus bs = read_battery_status();

  char response[384];
  snprintf(response, sizeof(response),
           "{\"cmd\":\"sleep\","
           "\"optimal_sec\":%u,"
           "\"battery_pct\":%d,"
           "\"normal\":%u,"
           "\"low_battery\":%u,"
           "\"critical\":%u,"
           "\"rapid_update\":%u,"
           "\"thresholds\":{\"low\":%u,\"critical\":%u}}",
           optimal, bs.percent, config.normal_interval_sec, config.low_battery_interval_sec,
           config.critical_interval_sec, config.rapid_update_interval_sec,
           config.low_battery_threshold, config.critical_battery_threshold);
  publishResponse(client, response);
}

void DebugCommands::cmdFeatures(PubSubClient* client) {
  char response[512];
  snprintf(response, sizeof(response),
           "{\"cmd\":\"features\","
           "\"ha_discovery\":%d,"
           "\"diagnostic_mode\":%d,"
           "\"status_pixel\":%d,"
           "\"pressure_sensor\":%d,"
           "\"outdoor_weather\":%d,"
           "\"battery_monitor\":%d,"
           "\"debug_commands\":%d,"
           "\"profiling\":%d,"
           "\"memory_tracking\":%d,"
           "\"crash_handler\":%d}",
           FEATURE_HA_DISCOVERY, FEATURE_DIAGNOSTIC_MODE, FEATURE_STATUS_PIXEL,
           FEATURE_PRESSURE_SENSOR, FEATURE_OUTDOOR_WEATHER, FEATURE_BATTERY_MONITOR,
           FEATURE_DEBUG_COMMANDS, FEATURE_PROFILING, FEATURE_MEMORY_TRACKING,
           FEATURE_CRASH_HANDLER);
  publishResponse(client, response);
}

void DebugCommands::cmdMqttBatch(PubSubClient* client) {
  char stats[192];
  MQTTBatcher::getInstance().formatStatsJson(stats, sizeof(stats));

  char response[256];
  snprintf(response, sizeof(response), "{\"cmd\":\"mqtt_batch\",%s}", json_object_body(stats));
  publishResponse(client, response);
}

void DebugCommands::cmdSmartRefresh(PubSubClient* client) {
  char stats[128];
  SmartRefresh::getInstance().formatStatsJson(stats, sizeof(stats));

  char response[256];
  // Safely merge JSON: skip opening brace only if stats starts with '{'
  const char* stats_content = (stats[0] == '{') ? stats + 1 : stats;
  snprintf(response, sizeof(response), "{\"cmd\":\"smart_refresh\",\"dirty_mask\":\"0x%04X\",%s}",
           SmartRefresh::getInstance().getDirtyMask(), stats_content);
  publishResponse(client, response);
}

void DebugCommands::cmdScreenshot(PubSubClient* client) {
#if USE_DISPLAY
  // Acknowledge the command first
  publishResponse(client, "{\"cmd\":\"screenshot\",\"status\":\"capturing\"}");

  // Call the display capture handler (it publishes the screenshot data)
  display_capture_handle("", 0);
#else
  publishResponse(client, "{\"cmd\":\"screenshot\",\"error\":\"Display not enabled\"}");
#endif
}

void DebugCommands::publishResponse(PubSubClient* client, const char* json) {
  if (!client || !client->connected())
    return;

  // Use static buffer instead of String to reduce heap fragmentation
  char topic[96];
  snprintf(topic, sizeof(topic), "espsensor/%s%s", client_id_, TOPIC_DEBUG_RESPONSE);
  client->publish(topic, json, false);
}

// C linkage for MQTT callback
extern "C" void debug_commands_handle(const char* topic, const uint8_t* payload, size_t length) {
  DebugCommands::getInstance().handleCommand(topic, payload, length);
}
