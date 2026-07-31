#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "logger.h"

// RAII wrapper for FreeRTOS mutex
class MutexGuard {
 public:
  explicit MutexGuard(SemaphoreHandle_t mutex) : mutex_(mutex), acquired_(false) {
    if (mutex_ && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
      acquired_ = true;
    }
  }

  ~MutexGuard() {
    if (acquired_ && mutex_) {
      xSemaphoreGive(mutex_);
    }
  }

  bool acquired() const { return acquired_; }

  // Prevent copying
  MutexGuard(const MutexGuard&) = delete;
  MutexGuard& operator=(const MutexGuard&) = delete;

 private:
  SemaphoreHandle_t mutex_;
  bool acquired_;
};

class LogBuffer {
 public:
  static constexpr size_t BUFFER_SIZE = 64;  // Reduced from 256 to fit in RTC memory

  static LogBuffer* getInstance();

  void begin();
  void end();

  bool push(const LogEntry& entry);

  bool pop(LogEntry& entry);

  bool getEntry(size_t index, LogEntry& entry) const;

  size_t getCount() const;

  bool isFull() const;
  bool isEmpty() const;

  void clear();

  size_t getCapacity() const { return BUFFER_SIZE; }

  uint32_t getOverflowCount() const { return overflow_count_; }
  void resetOverflowCount() { overflow_count_ = 0; }

  void dump(void (*output_fn)(const LogEntry&));

 private:
  LogBuffer() = default;
  ~LogBuffer() = default;
  LogBuffer(const LogBuffer&) = delete;
  LogBuffer& operator=(const LogBuffer&) = delete;

  // RTC_DATA_ATTR lives on the definitions (log_buffer.cpp) only: GCC 14
  // rejects the attribute appearing on both declaration and definition with
  // different .rtc.data section counters.
  static LogEntry buffer_[BUFFER_SIZE];
  static size_t head_;
  static size_t tail_;
  static size_t count_;
  static uint32_t overflow_count_;
  static bool wrapped_;

  mutable SemaphoreHandle_t mutex_ = nullptr;
  bool initialized_ = false;

  bool pushUnsafe(const LogEntry& entry);
  bool popUnsafe(LogEntry& entry);
};
