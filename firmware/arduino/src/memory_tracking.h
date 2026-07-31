#pragma once

#include <Arduino.h>
#include <esp_system.h>
#include <esp_heap_caps.h>

// Memory high watermark tracking system
// Tracks peak memory usage and allocation patterns
// Stores data in RTC memory to persist across deep sleep
//
// Usage:
//   MemoryTracker::getInstance().begin();
//
//   // Periodically update
//   MemoryTracker::getInstance().update();
//
//   // Query stats
//   auto stats = MemoryTracker::getInstance().getStats();
//   Serial.printf("Peak heap used: %u bytes\n", stats.heap_high_watermark);

class MemoryTracker {
 public:
  struct MemoryStats {
    uint32_t heap_high_watermark;   // Maximum heap usage seen
    uint32_t heap_low_watermark;    // Minimum free heap seen
    uint32_t stack_high_watermark;  // Maximum stack usage (estimated)
    uint32_t largest_allocation;    // Largest single allocation seen
    uint32_t total_allocations;     // Total allocation count (resets on reboot)
    uint32_t failed_allocations;    // Failed allocation count (resets on reboot)
    uint32_t fragmentation_peak;    // Worst fragmentation seen (%)
    uint32_t total_heap_size;       // Total heap size
    uint32_t psram_high_watermark;  // PSRAM peak usage (if available)
  };

  static MemoryTracker& getInstance();

  void begin();

  // Update watermarks (call periodically, e.g., in loop or before sleep)
  void update();

  // Record an allocation attempt

  // Get current statistics
  const MemoryStats& getStats() const { return stats_; }

  // Reset runtime counters (keeps watermarks)
  void resetCounters();

  // Reset all stats including watermarks
  void resetAll();

  // Format stats as JSON
  void formatStatsJson(char* out, size_t out_size) const;

  // Calculate current fragmentation percentage
  float getCurrentFragmentation() const;

 private:
  MemoryTracker();
  ~MemoryTracker() = default;
  MemoryTracker(const MemoryTracker&) = delete;
  MemoryTracker& operator=(const MemoryTracker&) = delete;

  // RTC memory storage (persists across deep sleep)
  RTC_DATA_ATTR static MemoryStats stats_;

  bool initialized_ = false;

  // Update heap watermarks
  void updateHeapWatermarks();

  // Estimate stack usage (ESP32-specific)
  uint32_t estimateStackUsage() const;
};
