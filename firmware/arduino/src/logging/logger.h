#pragma once

#include <Arduino.h>
#include <esp_timer.h>
#include <cstdarg>

enum class LogLevel : uint8_t {
  TRACE = 0,
  DEBUG = 1,
  INFO = 2,
  WARN = 3,
  ERROR = 4,
  FATAL = 5,
  NONE = 6
};

struct LogEntry {
  uint32_t timestamp;
  LogLevel level;
  uint8_t module_id;
  uint16_t sequence;
  char message[48];
} __attribute__((packed));

class Logger {
 public:
  static constexpr size_t MAX_MESSAGE_LENGTH = 128;
  static constexpr size_t MAX_MODULE_NAME_LENGTH = 16;

  struct Config {
    LogLevel min_level;
    bool serial_enabled;
    bool buffer_enabled;
    bool nvs_enabled;
    bool mqtt_enabled;
    uint16_t mqtt_rate_limit_ms;
    uint16_t enabled_modules_mask;  // Bitmask of enabled modules (0xFFFF = all enabled)

    Config()
        : min_level(LogLevel::INFO),
          serial_enabled(true),
          buffer_enabled(true),
          nvs_enabled(false),
          mqtt_enabled(false),
          mqtt_rate_limit_ms(1000),
          enabled_modules_mask(0xFFFF) {}  // All modules enabled by default
  };

  static Logger& getInstance();

  void begin(const Config& config = Config());
  void end();

  uint8_t registerModule(const char* name);

  void log(LogLevel level, uint8_t module, const char* format, ...);
  void logv(LogLevel level, uint8_t module, const char* format, va_list args);

  void trace(uint8_t module, const char* format, ...);
  void debug(uint8_t module, const char* format, ...);
  void info(uint8_t module, const char* format, ...);
  void warn(uint8_t module, const char* format, ...);
  void error(uint8_t module, const char* format, ...);
  void fatal(uint8_t module, const char* format, ...);

  void setLevel(LogLevel level);
  LogLevel getLevel() const { return config_.min_level; }

  void enableSerial(bool enable) { config_.serial_enabled = enable; }
  void enableBuffer(bool enable) { config_.buffer_enabled = enable; }
  void enableNVS(bool enable) { config_.nvs_enabled = enable; }
  void enableMQTT(bool enable) { config_.mqtt_enabled = enable; }

  // Module filtering
  void enableModule(uint8_t module_id);
  void disableModule(uint8_t module_id);
  void enableAllModules();
  void disableAllModules();
  bool isModuleEnabled(uint8_t module_id) const;
  uint8_t getModuleId(const char* name) const;
  const char* getModuleName(uint8_t module_id) const;
  uint8_t getModuleCount() const { return module_count_; }

  // Configuration via JSON (for MQTT commands)
  bool applyConfigJson(const char* json);
  void getConfigJson(char* out, size_t out_size) const;

  const char* levelToString(LogLevel level) const;
  LogLevel stringToLevel(const char* str) const;

  void flush();

  void dumpCrashLog();
  void clearCrashLog();

  size_t getBufferedCount() const;
  bool getBufferedEntry(size_t index, LogEntry& entry) const;

  uint32_t getDroppedCount() const { return dropped_count_; }
  void resetDroppedCount() { dropped_count_ = 0; }

 private:
  Logger() = default;
  ~Logger() = default;
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  void outputSerial(const LogEntry& entry);
  void outputBuffer(const LogEntry& entry);
  void outputNVS(const LogEntry& entry);
  void outputMQTT(const LogEntry& entry);

  Config config_;
  // Note: module_names_ initialized in begin() via memset - not in constructor by design
  // This is intentional for singleton pattern to avoid static initialization order issues
  char module_names_[16][MAX_MODULE_NAME_LENGTH];  // cppcheck-suppress uninitMemberVarPrivate
  uint8_t module_count_ = 0;
  uint16_t sequence_ = 0;
  uint32_t dropped_count_ = 0;
  uint32_t last_mqtt_time_ = 0;
  bool initialized_ = false;
};

// This module's macros are prefixed LOGM_ ("module log") rather than LOG_.
//
// They used to be called LOG_TRACE/LOG_DEBUG/... which collided with the
// Serial.printf-based macros of the same name in ../logging.h. Any translation
// unit that pulled in both — easily done, since safe_strings.h includes
// logging.h and profiling.h includes this header — got 20 "macro redefined"
// warnings, and whichever header happened to be included second silently
// decided which logging backend every subsequent LOG_ call in that file used.
//
// The two APIs are not interchangeable, so they must not share names:
//   LOG_*  (logging.h) unconditional Serial.printf, filtered at compile time by
//          LOG_LEVEL, and LOG_ERROR prefixes __FUNCTION__:__LINE__.
//   LOGM_* (here)      routed through Logger, needs a LOG_MODULE("name") in
//          scope, filtered at *runtime* by Logger's min_level, and additionally
//          feeds the ring buffer / NVS / MQTT sinks. Messages are truncated to
//          LogEntry::message (48 bytes).
#define LOG_MODULE(name) static uint8_t log_module_id = Logger::getInstance().registerModule(name)

#define LOGM_TRACE(fmt, ...) Logger::getInstance().trace(log_module_id, fmt, ##__VA_ARGS__)
#define LOGM_DEBUG(fmt, ...) Logger::getInstance().debug(log_module_id, fmt, ##__VA_ARGS__)
#define LOGM_INFO(fmt, ...) Logger::getInstance().info(log_module_id, fmt, ##__VA_ARGS__)
#define LOGM_WARN(fmt, ...) Logger::getInstance().warn(log_module_id, fmt, ##__VA_ARGS__)
#define LOGM_ERROR(fmt, ...) Logger::getInstance().error(log_module_id, fmt, ##__VA_ARGS__)
#define LOGM_FATAL(fmt, ...) Logger::getInstance().fatal(log_module_id, fmt, ##__VA_ARGS__)

#define LOG_LEVEL_SET(level) Logger::getInstance().setLevel(level)
#define LOG_FLUSH() Logger::getInstance().flush()
#define LOG_DUMP_CRASH() Logger::getInstance().dumpCrashLog()
#define LOG_CLEAR_CRASH() Logger::getInstance().clearCrashLog()
