#pragma once

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <queue>
#include "logger.h"

// External C interface for MQTT callback
extern "C" void log_mqtt_handle_command(const char* topic, const uint8_t* payload, size_t length);

class LogMQTT {
 public:
  static constexpr size_t MAX_QUEUE_SIZE = 32;
  static constexpr size_t JSON_BUFFER_SIZE = 256;
  static constexpr const char* TOPIC_PREFIX = "espsensor/";
  static constexpr const char* TOPIC_LOGS = "/logs";
  static constexpr const char* TOPIC_CMD_CLEAR = "/cmd/clear_logs";
  static constexpr const char* TOPIC_CMD_LEVEL = "/cmd/log_level";
  static constexpr const char* TOPIC_CMD_FILTER = "/cmd/log_filter";
  static constexpr const char* TOPIC_STATUS_CONFIG = "/debug/log_config";

  static LogMQTT* getInstance();

  void begin();
  void end();

  void setClientId(const char* id);

  bool publish(const LogEntry& entry, const char* module_name);

  void flush();

  bool isConnected() const;

  void setEnabled(bool enabled) { enabled_ = enabled; }
  bool isEnabled() const { return enabled_; }

  size_t getQueuedCount() const { return queue_.size(); }
  size_t getPublishedCount() const { return published_count_; }
  size_t getDroppedCount() const { return dropped_count_; }

  void resetCounters() {
    published_count_ = 0;
    dropped_count_ = 0;
  }

  void handleCommand(const char* topic, const uint8_t* payload, size_t length);

 private:
  LogMQTT() = default;
  ~LogMQTT() = default;
  LogMQTT(const LogMQTT&) = delete;
  LogMQTT& operator=(const LogMQTT&) = delete;

  struct QueuedEntry {
    LogEntry entry;
    char module_name[16];
  };

  std::queue<QueuedEntry> queue_;
  char client_id_[40] = {0};
  bool initialized_ = false;
  bool enabled_ = true;

  size_t published_count_ = 0;
  size_t dropped_count_ = 0;

  PubSubClient* getMQTTClient();

  bool publishQueued();

  String buildTopic(LogLevel level);
  String formatEntry(const LogEntry& entry, const char* module_name);
};
