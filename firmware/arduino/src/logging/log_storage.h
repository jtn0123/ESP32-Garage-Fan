#pragma once

#include "logger.h"
#include <Preferences.h>

class LogStorage {
 public:
  static constexpr size_t MAX_STORED_ENTRIES = 50;
  static constexpr const char* NVS_NAMESPACE = "logs";
  static constexpr const char* KEY_COUNT = "count";
  static constexpr const char* KEY_HEAD = "head";
  static constexpr const char* KEY_CRASH = "crash";
  static constexpr const char* KEY_PREFIX = "e_";

  static LogStorage* getInstance();

  void begin();
  void end();

  bool storeEntry(const LogEntry& entry);

  bool getEntry(size_t index, LogEntry& entry);

  size_t getStoredCount();

  void clearLogs();

  void markCrash();
  bool wasCrashed();
  void clearCrashFlag();

  void flush();

  void dumpToSerial();

 private:
  LogStorage() = default;
  ~LogStorage() = default;
  LogStorage(const LogStorage&) = delete;
  LogStorage& operator=(const LogStorage&) = delete;

  Preferences prefs_;
  bool initialized_ = false;
  size_t head_ = 0;
  size_t count_ = 0;
  bool dirty_ = false;

  void loadMetadata();
  void saveMetadata();

  String getEntryKey(size_t index);

  bool storeEntryAtIndex(size_t index, const LogEntry& entry);
  bool loadEntryAtIndex(size_t index, LogEntry& entry);

  void rotateIfNeeded();
};
