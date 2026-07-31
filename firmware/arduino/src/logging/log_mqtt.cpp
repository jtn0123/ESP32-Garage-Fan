#include "log_mqtt.h"
#include "../mqtt_client.h"
#include "../safe_strings.h"

extern PubSubClient* mqtt_get_client();

LogMQTT* LogMQTT::getInstance() {
  static LogMQTT instance;
  return &instance;
}

void LogMQTT::begin() {
  if (initialized_)
    return;

  while (!queue_.empty()) {
    queue_.pop();
  }

  published_count_ = 0;
  dropped_count_ = 0;

  initialized_ = true;
}

void LogMQTT::end() {
  if (!initialized_)
    return;

  flush();

  initialized_ = false;
}

void LogMQTT::setClientId(const char* id) {
  if (id) {
    safe_strcpy(client_id_, id);
  }
}

bool LogMQTT::publish(const LogEntry& entry, const char* module_name) {
  if (!initialized_ || !enabled_)
    return false;

  if (queue_.size() >= MAX_QUEUE_SIZE) {
    queue_.pop();
    dropped_count_++;
  }

  QueuedEntry queued;
  queued.entry = entry;
  safe_strcpy(queued.module_name, module_name);

  queue_.push(queued);

  if (isConnected()) {
    return publishQueued();
  }

  return true;
}

void LogMQTT::flush() {
  if (!initialized_)
    return;

  while (!queue_.empty() && isConnected()) {
    publishQueued();
  }
}

bool LogMQTT::isConnected() const {
  PubSubClient* client = mqtt_get_client();
  return client && client->connected();
}

PubSubClient* LogMQTT::getMQTTClient() { return mqtt_get_client(); }

bool LogMQTT::publishQueued() {
  if (queue_.empty())
    return true;

  PubSubClient* client = getMQTTClient();
  if (!client || !client->connected())
    return false;

  QueuedEntry& queued = queue_.front();

  String topic = buildTopic(queued.entry.level);
  String payload = formatEntry(queued.entry, queued.module_name);

  bool success = client->publish(topic.c_str(), payload.c_str(), false);

  if (success) {
    queue_.pop();
    published_count_++;
  }

  return success;
}

String LogMQTT::buildTopic(LogLevel level) {
  String topic = String(TOPIC_PREFIX) + client_id_ + TOPIC_LOGS;

  switch (level) {
    case LogLevel::ERROR:
    case LogLevel::FATAL:
      topic += "/error";
      break;
    case LogLevel::WARN:
      topic += "/warn";
      break;
    case LogLevel::INFO:
      topic += "/info";
      break;
    default:
      topic += "/debug";
      break;
  }

  return topic;
}

String LogMQTT::formatEntry(const LogEntry& entry, const char* module_name) {
  JsonDocument doc;

  doc["ts"] = entry.timestamp;
  doc["lvl"] = Logger::getInstance().levelToString(entry.level);
  doc["mod"] = module_name;
  doc["seq"] = entry.sequence;
  doc["msg"] = entry.message;

  String output;
  serializeJson(doc, output);

  return output;
}

void LogMQTT::handleCommand(const char* topic, const uint8_t* payload, size_t length) {
  if (!initialized_)
    return;

  String topic_str(topic);

  if (topic_str.endsWith(TOPIC_CMD_CLEAR)) {
    Logger::getInstance().clearCrashLog();

    while (!queue_.empty()) {
      queue_.pop();
    }

    resetCounters();

  } else if (topic_str.endsWith(TOPIC_CMD_LEVEL)) {
    if (length > 0 && length < 10) {
      char level_str[10];
      memcpy(level_str, payload, length);
      level_str[length] = '\0';

      LogLevel level = Logger::getInstance().stringToLevel(level_str);
      if (level != LogLevel::NONE) {
        Logger::getInstance().setLevel(level);
      }
    }

  } else if (topic_str.endsWith(TOPIC_CMD_FILTER)) {
    // Handle log filter configuration
    // Payload format: {"level":"DEBUG","modules":["MQTT","DISPLAY"],"serial":true}
    if (length > 0 && length < 512) {
      char config_json[512];
      memcpy(config_json, payload, length);
      config_json[length] = '\0';

      if (Logger::getInstance().applyConfigJson(config_json)) {
        // Publish current config as confirmation
        char response[256];
        Logger::getInstance().getConfigJson(response, sizeof(response));

        PubSubClient* client = getMQTTClient();
        if (client && client->connected()) {
          String status_topic = String(TOPIC_PREFIX) + client_id_ + TOPIC_STATUS_CONFIG;
          client->publish(status_topic.c_str(), response, false);
        }
      }
    }
  }
}

// External linkage function for MQTT callback
extern "C" void log_mqtt_handle_command(const char* topic, const uint8_t* payload, size_t length) {
  LogMQTT::getInstance()->handleCommand(topic, payload, length);
}
