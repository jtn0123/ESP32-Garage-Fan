// Logging infrastructure for ESP32 Temperature Sensor
#pragma once

#include <Arduino.h>
#include "generated_config.h"

// Log levels - compile-time configurable (legacy).
//
// These MUST be #defines, not an enum: they are compared in `#if LOG_LEVEL >=
// LOG_LEVEL_X` guards below, and the preprocessor substitutes 0 for any
// identifier it does not know — enum constants included. As an enum, every
// guard reduced to `0 >= 0` and no build flag could ever strip a log level.
#define LOG_LEVEL_ERROR 0
#define LOG_LEVEL_WARN 1
#define LOG_LEVEL_INFO 2
#define LOG_LEVEL_DEBUG 3
#define LOG_LEVEL_VERBOSE 4

// Set default log level based on build type
#ifndef LOG_LEVEL
#if defined(DEBUG_BUILD) || defined(DEV_NO_SLEEP)
#define LOG_LEVEL LOG_LEVEL_DEBUG
#elif defined(RELEASE_BUILD)
#define LOG_LEVEL LOG_LEVEL_WARN
#else
#define LOG_LEVEL LOG_LEVEL_INFO
#endif
#endif

// Mirror of the serial log to the microSD card. Defined in sd_store.cpp, and
// declared here rather than pulled in via sd_store.h so this header stays free of
// FS.h/SD.h — it is included (directly or through safe_strings.h) by most of the
// firmware, and the native tests build against it.
//
// A no-op until sd_log_set_mirror(true), which app_controller calls only after the
// card is mounted and `storage.logs_enabled` has been read. Only WARN and ERROR
// are mirrored: INFO is where the boot-time chatter lives, and an SD write costs
// milliseconds where a Serial.printf costs microseconds.
void log_mirror(const char* level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Compile-time log level filtering macros
#if LOG_LEVEL >= LOG_LEVEL_ERROR
#define LOG_ERROR(fmt, ...)                                                          \
  do {                                                                               \
    Serial.printf("[ERROR] %s:%d " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__); \
    log_mirror("ERROR", "%s:%d " fmt, __FUNCTION__, __LINE__, ##__VA_ARGS__);        \
  } while (0)
#else
#define LOG_ERROR(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_WARN
#define LOG_WARN(fmt, ...)                            \
  do {                                                \
    Serial.printf("[WARN] " fmt "\n", ##__VA_ARGS__); \
    log_mirror("WARN", fmt, ##__VA_ARGS__);           \
  } while (0)
#else
#define LOG_WARN(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_INFO
#define LOG_INFO(fmt, ...) Serial.printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG_INFO(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_DEBUG
#define LOG_DEBUG(fmt, ...) Serial.printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_VERBOSE
#define LOG_VERBOSE(fmt, ...) Serial.printf("[VERBOSE] " fmt "\n", ##__VA_ARGS__)
#else
#define LOG_VERBOSE(fmt, ...) ((void)0)
#endif

// Memory-safe logging helpers
inline void log_heap_status(const char* context) {
#if LOG_LEVEL >= LOG_LEVEL_DEBUG
  Serial.printf("[MEM] %s: Free=%d, Largest=%d\n", context, ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());
#endif
}

// Log with automatic memory status
#define LOG_WITH_MEM(level, fmt, ...) \
  do {                                \
    LOG_##level(fmt, ##__VA_ARGS__);  \
    log_heap_status(#level);          \
  } while (0)

// Performance logging helpers
#define LOG_TIMING_START(name) uint32_t _timing_##name = millis()
#define LOG_TIMING_END(name) LOG_DEBUG("Timing %s: %dms", #name, millis() - _timing_##name)

// Conditional logging based on change
// Note: Some branches may be unreachable depending on LOG_LEVEL compile-time value
// This is intentional - function supports all levels but only active levels are compiled in
template <typename T>
inline void log_if_changed(const char* name, T& last_value, T current_value,
                           int level = LOG_LEVEL_INFO) {
  if (last_value != current_value) {
    // cppcheck-suppress knownConditionTrueFalse
    if (level <= LOG_LEVEL) {
      Serial.printf("[%s] %s changed: ",
                    level == LOG_LEVEL_ERROR  ? "ERROR"
                    : level == LOG_LEVEL_WARN ? "WARN"
                    : level == LOG_LEVEL_INFO ? "INFO"
                    : level == LOG_LEVEL_DEBUG
                        ? "DEBUG"
                        : "VERBOSE",  // cppcheck-suppress knownConditionTrueFalse
                    name);
      Serial.println(current_value);
    }
    last_value = current_value;
  }
}

// Log function entry/exit for debugging
#if LOG_LEVEL >= LOG_LEVEL_VERBOSE
#define LOG_FUNCTION_ENTRY() LOG_VERBOSE("-> %s", __FUNCTION__)
#define LOG_FUNCTION_EXIT() LOG_VERBOSE("<- %s", __FUNCTION__)
#else
#define LOG_FUNCTION_ENTRY() ((void)0)
#define LOG_FUNCTION_EXIT() ((void)0)
#endif

// Assert with logging
#define LOG_ASSERT(cond, fmt, ...)                     \
  do {                                                 \
    if (!(cond)) {                                     \
      LOG_ERROR("ASSERT FAILED: " fmt, ##__VA_ARGS__); \
      abort();                                         \
    }                                                  \
  } while (0)

// Hexdump for debugging
inline void log_hexdump(const char* label, const uint8_t* data, size_t len) {
#if LOG_LEVEL >= LOG_LEVEL_DEBUG
  Serial.printf("[HEXDUMP] %s (%d bytes):\n", label, len);
  for (size_t i = 0; i < len; i++) {
    if (i % 16 == 0)
      Serial.printf("%04x: ", i);
    Serial.printf("%02x ", data[i]);
    if ((i + 1) % 16 == 0 || i == len - 1)
      Serial.println();
  }
#endif
}
