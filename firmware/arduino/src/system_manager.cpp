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

// Load cache values if unset (moved from main.cpp)

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
