#pragma once

#include <Arduino.h>
#include <PubSubClient.h>

// Debug command handler for remote debugging via MQTT
// Provides runtime inspection and control without serial connection
//
// Command topic: espsensor/<device_id>/cmd/debug
//
// Supported commands:
// - {"cmd": "heap"}                    -> Returns heap statistics
// - {"cmd": "state"}                   -> Returns current sensor state
// - {"cmd": "config"}                  -> Returns configuration
// - {"cmd": "restart"}                 -> Performs soft restart
// - {"cmd": "modules"}                 -> Lists all registered log modules
// - {"cmd": "uptime"}                  -> Returns system uptime
// - {"cmd": "network"}                 -> Returns WiFi/MQTT status
// - {"cmd": "sensors"}                 -> Returns sensor readings
// - {"cmd": "perf"}                    -> Returns performance profiling stats
// - {"cmd": "perf_reset"}              -> Resets performance counters
// - {"cmd": "crash"}                   -> Returns crash diagnostics
// - {"cmd": "crash_clear"}             -> Clears crash information
// - {"cmd": "memory"}                  -> Returns memory tracking stats
// - {"cmd": "memory_reset"}            -> Resets memory tracking stats
// - {"cmd": "sleep"}                   -> Returns adaptive sleep configuration
// - {"cmd": "features"}                -> Returns enabled/disabled features
// - {"cmd": "mqtt_batch"}              -> Returns MQTT batching statistics
// - {"cmd": "smart_refresh"}           -> Returns smart refresh statistics
// - {"cmd": "screenshot"}              -> Captures and publishes display screenshot

class DebugCommands {
 public:
  static constexpr const char* TOPIC_CMD_DEBUG = "/cmd/debug";
  static constexpr const char* TOPIC_DEBUG_RESPONSE = "/debug/response";

  static DebugCommands& getInstance();

  void begin();
  void setClientId(const char* client_id);

  // Handle debug command from MQTT
  void handleCommand(const char* topic, const uint8_t* payload, size_t length);

 private:
  DebugCommands() = default;
  ~DebugCommands() = default;
  DebugCommands(const DebugCommands&) = delete;
  DebugCommands& operator=(const DebugCommands&) = delete;

  char client_id_[40];
  bool initialized_ = false;

  // Command handlers
  void cmdHeap(PubSubClient* client);
  void cmdState(PubSubClient* client);
  void cmdConfig(PubSubClient* client);
  void cmdRestart(PubSubClient* client);
  void cmdModules(PubSubClient* client);
  void cmdUptime(PubSubClient* client);
  void cmdNetwork(PubSubClient* client);
  void cmdSensors(PubSubClient* client);
  void cmdPerf(PubSubClient* client);
  void cmdPerfReset(PubSubClient* client);
  void cmdCrash(PubSubClient* client);
  void cmdCrashClear(PubSubClient* client);
  void cmdMemory(PubSubClient* client);
  void cmdMemoryReset(PubSubClient* client);
  void cmdSleep(PubSubClient* client);
  void cmdFeatures(PubSubClient* client);
  void cmdMqttBatch(PubSubClient* client);
  void cmdSmartRefresh(PubSubClient* client);
  void cmdScreenshot(PubSubClient* client);

  // Helper to publish response
  void publishResponse(PubSubClient* client, const char* json);
};

// C linkage for MQTT callback
extern "C" void debug_commands_handle(const char* topic, const uint8_t* payload, size_t length);
