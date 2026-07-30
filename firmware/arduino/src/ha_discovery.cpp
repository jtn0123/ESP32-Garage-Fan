// Home Assistant MQTT discovery implementation - extracted from net.h
#include "ha_discovery.h"
#include <cstdio>
#include "feature_flags.h"

#if FEATURE_HA_DISCOVERY

#include "mqtt_client.h"
#include "generated_config.h"
#include "config.h"
#include "runtime_config.h"

// Forward declaration for MQTT publish function
extern bool mqtt_publish_raw(const char* topic, const char* payload, bool retain);

static char g_device_id[40];
static bool g_diagnostic_entities = false;

// Helper to publish discovery config
static void publish_discovery_config(const char* component, const char* sensor_name,
                                     const char* json_config) {
  char topic[256];
  snprintf(topic, sizeof(topic), "homeassistant/%s/%s_%s/config", component, g_device_id,
           sensor_name);

  mqtt_publish_raw(topic, json_config, true);
}

// Helper to build device JSON
static String build_device_json() {
  return String("{") + "\"identifiers\":[\"" + g_device_id + "\"]," + "\"name\":\"" +
         rc_room_name() + " Sensor\"," + "\"model\":\"ESP32 Environmental Sensor\"," +
         "\"manufacturer\":\"DIY\"," + "\"sw_version\":\"" + FW_VERSION + "\"" + "}";
}

void ha_discovery_begin(const char* client_id) {
  if (client_id) {
    snprintf(g_device_id, sizeof(g_device_id), "%s", client_id);
  }
}

void ha_discovery_publish_all() {
  if (!mqtt_is_connected())
    return;

  ha_discovery_publish_temperature_sensor();
  ha_discovery_publish_humidity_sensor();
  ha_discovery_publish_pressure_sensor();
  ha_discovery_publish_battery_sensor();
  ha_discovery_publish_rssi_sensor();

  if (g_diagnostic_entities) {
    ha_discovery_publish_diagnostic_sensors();
  }
}

void ha_discovery_publish_temperature_sensor() {
  if (!mqtt_is_connected())
    return;

  String config =
      String("{") + "\"name\":\"" + rc_room_name() + " Temperature\"," + "\"unique_id\":\"" +
      g_device_id + "_temperature\"," + "\"state_topic\":\"espsensor/" + g_device_id +
      "/inside/temperature\"," + "\"availability_topic\":\"espsensor/" + g_device_id +
      "/availability\"," + "\"device_class\":\"temperature\"," + "\"unit_of_measurement\":\"°C\"," +
      "\"value_template\":\"{{ value | round(1) }}\"," + "\"device\":" + build_device_json() + "}";

  publish_discovery_config("sensor", "temperature", config.c_str());
}

void ha_discovery_publish_humidity_sensor() {
  if (!mqtt_is_connected())
    return;

  String config =
      String("{") + "\"name\":\"" + rc_room_name() + " Humidity\"," + "\"unique_id\":\"" +
      g_device_id + "_humidity\"," + "\"state_topic\":\"espsensor/" + g_device_id +
      "/inside/humidity\"," + "\"availability_topic\":\"espsensor/" + g_device_id +
      "/availability\"," + "\"device_class\":\"humidity\"," + "\"unit_of_measurement\":\"%\"," +
      "\"value_template\":\"{{ value | round(1) }}\"," + "\"device\":" + build_device_json() + "}";

  publish_discovery_config("sensor", "humidity", config.c_str());
}

void ha_discovery_publish_pressure_sensor() {
  if (!mqtt_is_connected())
    return;

  String config =
      String("{") + "\"name\":\"" + rc_room_name() + " Pressure\"," + "\"unique_id\":\"" +
      g_device_id + "_pressure\"," + "\"state_topic\":\"espsensor/" + g_device_id +
      "/inside/pressure\"," + "\"availability_topic\":\"espsensor/" + g_device_id +
      "/availability\"," + "\"device_class\":\"atmospheric_pressure\"," +
      "\"unit_of_measurement\":\"hPa\"," + "\"value_template\":\"{{ value | round(1) }}\"," +
      "\"device\":" + build_device_json() + "}";

  publish_discovery_config("sensor", "pressure", config.c_str());
}

void ha_discovery_publish_battery_sensor() {
  if (!mqtt_is_connected())
    return;

  // Battery percentage
  String config = String("{") + "\"name\":\"" + rc_room_name() + " Battery\"," +
                  "\"unique_id\":\"" + g_device_id + "_battery\"," +
                  "\"state_topic\":\"espsensor/" + g_device_id + "/battery/percent\"," +
                  "\"availability_topic\":\"espsensor/" + g_device_id + "/availability\"," +
                  "\"device_class\":\"battery\"," + "\"unit_of_measurement\":\"%\"," +
                  "\"device\":" + build_device_json() + "}";

  publish_discovery_config("sensor", "battery", config.c_str());

  // Battery voltage (diagnostic)
  if (g_diagnostic_entities) {
    config = String("{") + "\"name\":\"" + rc_room_name() + " Battery Voltage\"," +
             "\"unique_id\":\"" + g_device_id + "_battery_voltage\"," +
             "\"state_topic\":\"espsensor/" + g_device_id + "/battery/voltage\"," +
             "\"availability_topic\":\"espsensor/" + g_device_id + "/availability\"," +
             "\"device_class\":\"voltage\"," + "\"unit_of_measurement\":\"V\"," +
             "\"entity_category\":\"diagnostic\"," + "\"device\":" + build_device_json() + "}";

    publish_discovery_config("sensor", "battery_voltage", config.c_str());
  }
}

void ha_discovery_publish_rssi_sensor() {
  if (!mqtt_is_connected())
    return;

  String config = String("{") + "\"name\":\"" + rc_room_name() + " WiFi RSSI\"," +
                  "\"unique_id\":\"" + g_device_id + "_rssi\"," + "\"state_topic\":\"espsensor/" +
                  g_device_id + "/wifi/rssi\"," + "\"availability_topic\":\"espsensor/" +
                  g_device_id + "/availability\"," + "\"device_class\":\"signal_strength\"," +
                  "\"unit_of_measurement\":\"dBm\"," + "\"entity_category\":\"diagnostic\"," +
                  "\"device\":" + build_device_json() + "}";

  publish_discovery_config("sensor", "rssi", config.c_str());
}

void ha_discovery_publish_diagnostic_sensors() {
  if (!mqtt_is_connected())
    return;

  // Uptime sensor
  String config = String("{") + "\"name\":\"" + rc_room_name() + " Uptime\"," + "\"unique_id\":\"" +
                  g_device_id + "_uptime\"," + "\"state_topic\":\"espsensor/" + g_device_id +
                  "/debug/uptime\"," + "\"availability_topic\":\"espsensor/" + g_device_id +
                  "/availability\"," + "\"unit_of_measurement\":\"s\"," +
                  "\"icon\":\"mdi:timer-outline\"," + "\"entity_category\":\"diagnostic\"," +
                  "\"device\":" + build_device_json() + "}";

  publish_discovery_config("sensor", "uptime", config.c_str());

  // Wake count sensor
  config = String("{") + "\"name\":\"" + rc_room_name() + " Wake Count\"," + "\"unique_id\":\"" +
           g_device_id + "_wake_count\"," + "\"state_topic\":\"espsensor/" + g_device_id +
           "/debug/wake_count\"," + "\"availability_topic\":\"espsensor/" + g_device_id +
           "/availability\"," + "\"icon\":\"mdi:counter\"," +
           "\"entity_category\":\"diagnostic\"," + "\"device\":" + build_device_json() + "}";

  publish_discovery_config("sensor", "wake_count", config.c_str());
}

void ha_discovery_set_diagnostic_mode(bool enable) { g_diagnostic_entities = enable; }

#else  // !FEATURE_HA_DISCOVERY

// No-op stubs when HA Discovery is disabled
void ha_discovery_begin(const char*) {}
void ha_discovery_publish_all() {}
void ha_discovery_publish_temperature_sensor() {}
void ha_discovery_publish_humidity_sensor() {}
void ha_discovery_publish_pressure_sensor() {}
void ha_discovery_publish_battery_sensor() {}
void ha_discovery_publish_rssi_sensor() {}
void ha_discovery_publish_diagnostic_sensors() {}
void ha_discovery_set_diagnostic_mode(bool) {}

#endif  // FEATURE_HA_DISCOVERY
