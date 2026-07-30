#pragma once

#include <Arduino.h>
#include <esp_timer.h>
#include <cstdio>
#include "logging/logger.h"
#include "feature_flags.h"

// Performance profiling instrumentation
// Provides automatic timing of code blocks and statistical tracking
//
// Usage:
//   void my_function() {
//       PROFILE_SCOPE("my_function");
//       // ... code to profile
//   }
//
// Configuration:
//   FEATURE_PROFILING (from feature_flags.h) - Enable profiling
//   #define SLOW_THRESHOLD_US 1000 - Log warning if operation exceeds this (microseconds)

#ifndef PROFILING_ENABLED
#define PROFILING_ENABLED FEATURE_PROFILING
#endif

#ifndef SLOW_THRESHOLD_US
#define SLOW_THRESHOLD_US 1000  // 1ms default threshold
#endif

#if PROFILING_ENABLED

struct PerfStats {
  const char* name;
  uint32_t count;
  uint32_t total_us;
  uint32_t min_us;
  uint32_t max_us;
  uint32_t last_us;

  PerfStats() : name(nullptr), count(0), total_us(0), min_us(UINT32_MAX), max_us(0), last_us(0) {}

  void record(uint32_t elapsed_us) {
    count++;
    total_us += elapsed_us;
    last_us = elapsed_us;
    if (elapsed_us < min_us)
      min_us = elapsed_us;
    if (elapsed_us > max_us)
      max_us = elapsed_us;
  }

  uint32_t getAverage() const { return count > 0 ? total_us / count : 0; }

  void reset() {
    count = 0;
    total_us = 0;
    min_us = UINT32_MAX;
    max_us = 0;
    last_us = 0;
  }
};

class PerformanceMonitor {
 public:
  static constexpr size_t MAX_STATS = 32;

  static PerformanceMonitor& getInstance() {
    static PerformanceMonitor instance;
    return instance;
  }

  // Get or create stats entry for the given name.
  // NOTE: The name pointer is stored directly - caller must ensure it points to
  // a string with static lifetime (e.g., __func__, string literals).
  // Do NOT pass dynamically allocated or stack-based strings.
  PerfStats* getStats(const char* name) {
    // Find existing stat - try pointer comparison first (fast path for __func__)
    // then fall back to strcmp for different pointers with same content
    for (size_t i = 0; i < stat_count_; i++) {
      if (stats_[i].name == name || strcmp(stats_[i].name, name) == 0) {
        return &stats_[i];
      }
    }

    // Create new stat
    if (stat_count_ < MAX_STATS) {
      stats_[stat_count_].name = name;  // Store pointer directly (must be static!)
      return &stats_[stat_count_++];
    }

    return nullptr;  // Out of slots
  }

  void record(const char* name, uint32_t elapsed_us) {
    PerfStats* stat = getStats(name);
    if (stat) {
      stat->record(elapsed_us);
    }
  }

  void reset() {
    for (size_t i = 0; i < stat_count_; i++) {
      stats_[i].reset();
    }
  }

  void resetAll() { stat_count_ = 0; }

  size_t getStatCount() const { return stat_count_; }
  PerfStats* getStatByIndex(size_t index) {
    return (index < stat_count_) ? &stats_[index] : nullptr;
  }

  // Format all stats to JSON
  void formatJson(char* out, size_t out_size) const {
    if (out_size == 0)
      return;

    size_t pos = 0;
    int written = snprintf(out, out_size, "{\"stats\":[");
    if (written < 0) {
      out[0] = '\0';
      return;
    }
    pos = static_cast<size_t>(written);

    for (size_t i = 0; i < stat_count_ && pos < out_size - 1; i++) {
      if (i > 0 && pos < out_size - 1) {
        written = snprintf(out + pos, out_size - pos, ",");
        if (written > 0)
          pos += static_cast<size_t>(written);
      }

      if (pos >= out_size - 1)
        break;

      const PerfStats& s = stats_[i];
      size_t remaining = out_size - pos;
      written = snprintf(
          out + pos, remaining,
          "{\"name\":\"%s\",\"count\":%u,\"avg_us\":%u,\"min_us\":%u,\"max_us\":%u,\"last_us\":%u}",
          s.name, s.count, s.getAverage(), s.min_us, s.max_us, s.last_us);
      // Check: written >= 0 means success, written < remaining means not truncated
      if (written >= 0 && static_cast<size_t>(written) < remaining) {
        pos += static_cast<size_t>(written);
      } else {
        break;  // Buffer full or error, stop adding entries
      }
    }

    if (pos < out_size - 2) {
      snprintf(out + pos, out_size - pos, "]}");
    } else {
      // Truncate gracefully - ensure valid JSON
      if (out_size >= 3) {
        out[out_size - 3] = ']';
        out[out_size - 2] = '}';
        out[out_size - 1] = '\0';
      }
    }
  }

 private:
  PerformanceMonitor() = default;
  ~PerformanceMonitor() = default;
  PerformanceMonitor(const PerformanceMonitor&) = delete;
  PerformanceMonitor& operator=(const PerformanceMonitor&) = delete;

  PerfStats stats_[MAX_STATS];
  size_t stat_count_ = 0;
};

class ScopedTimer {
 public:
  explicit ScopedTimer(const char* name) : name_(name), start_(esp_timer_get_time()) {}

  ~ScopedTimer() {
    uint64_t end = esp_timer_get_time();
    uint32_t elapsed = static_cast<uint32_t>(end - start_);

    // Record in performance monitor
    PerformanceMonitor::getInstance().record(name_, elapsed);

    // Log warning if slow
    if (elapsed > SLOW_THRESHOLD_US) {
      LOG_MODULE("PERF");
      LOGM_WARN("SLOW: %s took %u us (%.2f ms)", name_, elapsed, elapsed / 1000.0f);
    }
  }

 private:
  const char* name_;
  uint64_t start_;
};

// Macro for easy profiling
#define PROFILE_SCOPE(name) ScopedTimer _timer_##__LINE__(name)

// Macro for conditional profiling (only if threshold exceeded)
#define PROFILE_SCOPE_SLOW(name, threshold_us) \
  ScopedTimerConditional _timer_##__LINE__(name, threshold_us)

// Conditional timer that only records if threshold exceeded
class ScopedTimerConditional {
 public:
  ScopedTimerConditional(const char* name, uint32_t threshold_us)
      : name_(name), threshold_(threshold_us), start_(esp_timer_get_time()) {}

  ~ScopedTimerConditional() {
    uint64_t end = esp_timer_get_time();
    uint32_t elapsed = static_cast<uint32_t>(end - start_);

    if (elapsed > threshold_) {
      PerformanceMonitor::getInstance().record(name_, elapsed);

      LOG_MODULE("PERF");
      LOGM_WARN("SLOW: %s took %u us (%.2f ms), threshold %u us", name_, elapsed, elapsed / 1000.0f,
                threshold_);
    }
  }

 private:
  const char* name_;
  uint32_t threshold_;
  uint64_t start_;
};

#else  // PROFILING_ENABLED == 0

// No-op implementations when profiling is disabled
#define PROFILE_SCOPE(name)
#define PROFILE_SCOPE_SLOW(name, threshold_us)

class PerformanceMonitor {
 public:
  static PerformanceMonitor& getInstance() {
    static PerformanceMonitor instance;
    return instance;
  }
  void record(const char*, uint32_t) {}
  void reset() {}
  void resetAll() {}
  size_t getStatCount() const { return 0; }
  void formatJson(char* out, size_t out_size) const {
    if (out_size > 0) {
      int written = snprintf(out, out_size, "{\"stats\":[],\"enabled\":false}");
      if (written < 0)
        out[0] = '\0';
    }
  }
};

#endif  // PROFILING_ENABLED
