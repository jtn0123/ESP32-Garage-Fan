#include "memory_tracking.h"
#include <esp_heap_caps.h>
#include <cstdio>

// RTC memory - persists across deep sleep
RTC_DATA_ATTR MemoryTracker::MemoryStats MemoryTracker::stats_ = {};

MemoryTracker::MemoryTracker() {
  // Constructor intentionally minimal - initialization in begin()
}

MemoryTracker& MemoryTracker::getInstance() {
  static MemoryTracker instance;
  return instance;
}

void MemoryTracker::begin() {
  if (initialized_)
    return;

  // Get total heap size (only needs to be done once)
  if (stats_.total_heap_size == 0) {
    stats_.total_heap_size = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
  }

  // Reset per-boot counters
  stats_.total_allocations = 0;
  stats_.failed_allocations = 0;

  // Initial update
  update();

  initialized_ = true;
}

void MemoryTracker::update() {
  updateHeapWatermarks();

  // Update stack watermark
  uint32_t stack_usage = estimateStackUsage();
  if (stack_usage > stats_.stack_high_watermark) {
    stats_.stack_high_watermark = stack_usage;
  }

  // Update fragmentation peak
  float current_frag = getCurrentFragmentation();
  uint32_t frag_pct = static_cast<uint32_t>(current_frag * 100.0f);
  if (frag_pct > stats_.fragmentation_peak) {
    stats_.fragmentation_peak = frag_pct;
  }

// Update PSRAM if available
#ifdef BOARD_HAS_PSRAM
  size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  if (psram_size > 0) {
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t psram_used = psram_size - psram_free;
    if (psram_used > stats_.psram_high_watermark) {
      stats_.psram_high_watermark = psram_used;
    }
  }
#endif
}

void MemoryTracker::resetCounters() {
  stats_.total_allocations = 0;
  stats_.failed_allocations = 0;
}

void MemoryTracker::resetAll() {
  stats_.heap_high_watermark = 0;
  stats_.heap_low_watermark = 0;
  stats_.stack_high_watermark = 0;
  stats_.largest_allocation = 0;
  stats_.total_allocations = 0;
  stats_.failed_allocations = 0;
  stats_.fragmentation_peak = 0;
  stats_.psram_high_watermark = 0;

  // Re-initialize
  update();
}

void MemoryTracker::formatStatsJson(char* out, size_t out_size) const {
  snprintf(out, out_size,
           "{\"heap_high_wm\":%u,"
           "\"heap_low_wm\":%u,"
           "\"stack_high_wm\":%u,"
           "\"largest_alloc\":%u,"
           "\"total_allocs\":%u,"
           "\"failed_allocs\":%u,"
           "\"frag_peak_pct\":%u,"
           "\"total_heap\":%u,"
           "\"psram_high_wm\":%u}",
           stats_.heap_high_watermark, stats_.heap_low_watermark, stats_.stack_high_watermark,
           stats_.largest_allocation, stats_.total_allocations, stats_.failed_allocations,
           stats_.fragmentation_peak, stats_.total_heap_size, stats_.psram_high_watermark);
}

float MemoryTracker::getCurrentFragmentation() const {
  uint32_t free_heap = esp_get_free_heap_size();
  if (free_heap == 0)
    return 100.0f;

  uint32_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

  // Sanity check: largest block can't exceed free heap
  // (could happen if heap state is inconsistent during measurement)
  if (largest_block > free_heap) {
    largest_block = free_heap;
  }

  // Fragmentation = (1 - largest_block / free_heap) * 100%
  // 0% = no fragmentation (largest block == free heap)
  // 100% = completely fragmented (no large blocks available)
  return 100.0f * (1.0f - static_cast<float>(largest_block) / static_cast<float>(free_heap));
}

void MemoryTracker::updateHeapWatermarks() {
  uint32_t free_heap = esp_get_free_heap_size();
  uint32_t used_heap = stats_.total_heap_size - free_heap;

  // Update high watermark (peak usage)
  if (used_heap > stats_.heap_high_watermark) {
    stats_.heap_high_watermark = used_heap;
  }

  // Update low watermark (minimum free)
  if (stats_.heap_low_watermark == 0 || free_heap < stats_.heap_low_watermark) {
    stats_.heap_low_watermark = free_heap;
  }
}

uint32_t MemoryTracker::estimateStackUsage() const {
// ESP32-specific stack estimation
// This is approximate - gets the current stack pointer distance from base

// Get stack base (approximate - varies by core and task)
#if CONFIG_FREERTOS_UNICORE
  const uint32_t STACK_BASE = 0x3FFB0000;  // Approximate for core 0
#else
  const uint32_t STACK_BASE = 0x3FFB8000;  // Approximate for SMP
#endif

  // Get current stack pointer (inline assembly)
  uint32_t sp;
  __asm__ __volatile__("mov %0, sp" : "=r"(sp));

  // Calculate usage (stack grows downward)
  if (sp < STACK_BASE) {
    return STACK_BASE - sp;
  }

  return 0;
}
