// System management implementation - extracted from main.cpp
#include "system_manager.h"
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include "config.h"
#include "generated_config.h"
#include "system_pure.h"
#ifdef LOG_ENABLED
#include "logging/logger.h"
#include "logging/log_buffer.h"
#include "logging/log_storage.h"
LOG_MODULE("SYSTEM");
#endif

// RTC memory for persisting data across deep sleep
RTC_DATA_ATTR static uint32_t rtc_wake_count = 0;

// Global preferences object for NVS cache
static Preferences g_prefs;

// Display deadline for timing
static uint32_t g_display_deadline_ms = 0;

// Get current wake count
uint32_t get_wake_count() { return rtc_wake_count; }

// Increment wake count
void increment_wake_count() { rtc_wake_count++; }

void reset_wake_count() { rtc_wake_count = 0; }

// Get memory diagnostics
MemoryDiagnostics get_memory_diagnostics() {
  MemoryDiagnostics diag;
  diag.free_heap = esp_get_free_heap_size();
  diag.min_free_heap = esp_get_minimum_free_heap_size();
  diag.largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

  // Calculate fragmentation percentage
  if (diag.free_heap > 0 && diag.largest_free_block > 0) {
    diag.fragmentation_pct = (static_cast<float>(diag.free_heap - diag.largest_free_block) /
                              static_cast<float>(diag.free_heap)) *
                             100.0f;
  } else {
    diag.fragmentation_pct = 0.0f;
  }

#ifdef CONFIG_SPIRAM
  diag.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  diag.psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
#else
  diag.psram_free = 0;
  diag.psram_size = 0;
#endif
  return diag;
}

// Print boot diagnostics
void print_boot_diagnostics() {
  Serial.println(F("\n=== Boot Diagnostics ==="));

  // Reset reason
  esp_reset_reason_t reset_reason = esp_reset_reason();
  Serial.print(F("Reset reason: "));
  Serial.println(get_reset_reason_string(reset_reason));

#ifdef LOG_ENABLED
  // Check for crash and dump logs if needed
  if (reset_reason_is_crash(reset_reason)) {
    LOGM_FATAL("System crashed with reason: %s", get_reset_reason_string(reset_reason));

    // Check if we have crash logs in NVS
    LogStorage* storage = LogStorage::getInstance();
    if (storage && storage->wasCrashed()) {
      Serial.println(F("Previous crash detected - dumping logs:"));
      Logger::getInstance().dumpCrashLog();
      storage->dumpToSerial();
      storage->clearCrashFlag();
    }
  }
#endif

  // Memory stats
  MemoryDiagnostics mem = get_memory_diagnostics();
  Serial.printf("Heap: free=%u min=%u\n", mem.free_heap, mem.min_free_heap);

  // Wake count
  Serial.printf("Wake count: %u\n", rtc_wake_count);

  Serial.println(F("========================"));
}

// Get reset reason as string
// Both of these now live in system_pure.h so the native unit tests can exercise
// the real logic instead of a copy of it. These wrappers keep the existing
// call sites unchanged.
const char* get_reset_reason_string(esp_reset_reason_t reason) {
  return reset_reason_to_string(reason);
}

// Check if reset reason indicates a crash
bool reset_reason_is_crash(esp_reset_reason_t reason) {
  return reset_reason_indicates_crash(reason);
}

// Go to deep sleep with wake tracking
void go_deep_sleep_with_tracking(uint32_t seconds) {
#ifdef LOG_ENABLED
  LOGM_INFO("Entering deep sleep for %u seconds. Wake count: %u", seconds, rtc_wake_count);
  Logger::getInstance().flush();
#endif

  Serial.printf("Entering deep sleep for %u seconds\n", seconds);
  Serial.flush();

  // Configure wake timer
  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);

  // Increment wake count for next boot
  rtc_wake_count++;

  // Enter deep sleep
  esp_deep_sleep_start();
}

// NVS cache operations
void nvs_begin_cache() { g_prefs.begin("cache", false); }

void nvs_end_cache() { g_prefs.end(); }

void nvs_store_float(const char* key, float value) { g_prefs.putFloat(key, value); }

void nvs_store_uint(const char* key, uint32_t value) { g_prefs.putUInt(key, value); }

void nvs_store_ushort(const char* key, uint16_t value) { g_prefs.putUShort(key, value); }

void nvs_store_uchar(const char* key, uint8_t value) { g_prefs.putUChar(key, value); }

float nvs_load_float(const char* key, float defaultValue) {
  return g_prefs.getFloat(key, defaultValue);
}

uint32_t nvs_load_uint(const char* key, uint32_t defaultValue) {
  return g_prefs.getUInt(key, defaultValue);
}

uint16_t nvs_load_ushort(const char* key, uint16_t defaultValue) {
  return g_prefs.getUShort(key, defaultValue);
}

uint8_t nvs_load_uchar(const char* key, uint8_t defaultValue) {
  return g_prefs.getUChar(key, defaultValue);
}

// Get/set display deadline
uint32_t get_display_deadline_ms() { return g_display_deadline_ms; }

void set_display_deadline_ms(uint32_t deadline) { g_display_deadline_ms = deadline; }

// Print memory statistics
void print_memory_stats() {
  MemoryDiagnostics mem = get_memory_diagnostics();
  Serial.printf("Free heap: %u bytes\n", mem.free_heap);
  Serial.printf("Min free heap: %u bytes\n", mem.min_free_heap);
  Serial.printf("Largest free block: %u bytes\n", mem.largest_free_block);
  Serial.printf("Fragmentation: %.1f%%\n", mem.fragmentation_pct);
  if (mem.psram_size > 0) {
    Serial.printf("PSRAM free: %u/%u bytes\n", mem.psram_free, mem.psram_size);
  }
}

// Load cache values if unset (moved from main.cpp)

// Handle serial command line (will be moved from main.cpp)
void handle_serial_command_line(const String& line) {
#ifdef LOG_ENABLED
  // Test logging commands
  if (line == "log test") {
    Serial.println(F("Running logging test..."));
    LOGM_TRACE("Test TRACE message");
    LOGM_DEBUG("Test DEBUG message");
    LOGM_INFO("Test INFO message with number: %d", 42);
    LOGM_WARN("Test WARNING message");
    LOGM_ERROR("Test ERROR message with code: %d", 500);
    Serial.println(F("Logging test complete - check serial output"));
    return;
  }

  if (line == "log dump") {
    Serial.println(F("Dumping crash log buffer..."));
    Logger::getInstance().dumpCrashLog();
    return;
  }

  if (line == "log stats") {
    LogBuffer* buffer = LogBuffer::getInstance();
    LogStorage* storage = LogStorage::getInstance();
    Serial.printf("Buffer: %zu/%zu entries, %u overflows\n", buffer->getCount(),
                  buffer->getCapacity(), buffer->getOverflowCount());
    Serial.printf("NVS: %zu stored entries\n", storage->getStoredCount());
    Serial.printf("Dropped logs: %u\n", Logger::getInstance().getDroppedCount());
    return;
  }

  if (line.startsWith("log level ")) {
    String level = line.substring(10);
    level.toUpperCase();
    LogLevel new_level = Logger::getInstance().stringToLevel(level.c_str());
    if (new_level != LogLevel::NONE) {
      Logger::getInstance().setLevel(new_level);
      Serial.printf("Log level set to: %s\n", level.c_str());
    } else {
      Serial.println(F("Invalid level. Use: TRACE, DEBUG, INFO, WARN, ERROR, FATAL"));
    }
    return;
  }

  if (line == "log help") {
    Serial.println(F("Logging commands:"));
    Serial.println(F("  log test   - Run logging test"));
    Serial.println(F("  log dump   - Dump crash log buffer"));
    Serial.println(F("  log stats  - Show logging statistics"));
    Serial.println(F("  log level [LEVEL] - Set log level"));
    Serial.println(F("  log help   - Show this help"));
    return;
  }
#endif

  // Original placeholder for other commands
  Serial.println(F("Unknown command. Try 'log help' for logging commands"));
}

// CRC32 calculation utility
uint32_t fast_crc32(const uint8_t* data, size_t len) {
  // Implementation lives in system_pure.h so it is unit-testable on the host.
  return crc32_bytes(data, len);
}

// Template implementations for conditional redraws
template <typename DrawFn>
bool maybe_redraw_numeric(const int rect[4], float currentValue, float& lastValue, float threshold,
                          DrawFn drawFn) {
  bool should = false;
  if (!isnan(currentValue) &&
      (!isfinite(lastValue) || fabsf(currentValue - lastValue) >= threshold))
    should = true;
  if (should) {
    drawFn();
    lastValue = currentValue;
    return true;
  }
  return false;
}

template <typename T, typename DrawFn>
bool maybe_redraw_value(const int rect[4], const T& currentValue, T& lastValue, DrawFn drawFn) {
  if (currentValue != lastValue) {
    drawFn();
    lastValue = currentValue;
    return true;
  }
  return false;
}

// Explicit instantiations for common types to avoid linker errors
template bool maybe_redraw_numeric<void (*)()>(const int*, float, float&, float, void (*)());
template bool maybe_redraw_value<int, void (*)()>(const int*, const int&, int&, void (*)());
template bool maybe_redraw_value<uint32_t, void (*)()>(const int*, const uint32_t&, uint32_t&,
                                                       void (*)());
