#pragma once

#include <Arduino.h>
#include <esp_system.h>
#include <rom/rtc.h>

// Crash diagnostics and recovery system
// Stores crash information in RTC memory to survive reboots
// Provides post-mortem analysis via MQTT
//
// Usage:
//   void setup() {
//       CrashHandler::getInstance().begin();
//
//       if (CrashHandler::getInstance().hasCrashInfo()) {
//           // Report crash via MQTT
//           char report[512];
//           CrashHandler::getInstance().formatCrashReport(report, sizeof(report));
//           mqtt_publish("crash", report);
//           CrashHandler::getInstance().clearCrashInfo();
//       }
//   }

class CrashHandler {
 public:
  static constexpr size_t MAX_FUNCTION_NAME = 32;
  static constexpr uint32_t CRASH_MAGIC = 0xDEADBEEF;

  struct CrashInfo {
    uint32_t magic;                         // Magic number to validate data
    uint32_t crash_count;                   // Total crash count
    uint32_t boot_count;                    // Total boot count
    uint32_t last_crash_timestamp;          // millis() at crash
    uint32_t last_crash_pc;                 // Program counter (not always available)
    uint32_t last_crash_sp;                 // Stack pointer (not always available)
    esp_reset_reason_t reset_reason;        // ESP32 reset reason
    RESET_REASON rtc_reset_reason;          // RTC reset reason (more detailed)
    char last_function[MAX_FUNCTION_NAME];  // Last logged function before crash
    uint32_t free_heap_at_crash;            // Heap at time of crash
    uint32_t min_free_heap;                 // Minimum free heap seen
    uint16_t checksum;                      // Checksum for data validation
  };

  static CrashHandler& getInstance();

  void begin();

  // Check if we have valid crash information
  bool hasCrashInfo() const;

  // Get crash information
  const CrashInfo& getCrashInfo() const { return crash_info_; }

  // Clear crash information
  void clearCrashInfo();

  // Record current function (for debugging)
  void recordFunction(const char* function_name);

  // Update heap statistics
  void updateHeapStats();

  // Format crash report as JSON
  void formatCrashReport(char* out, size_t out_size) const;

  // Get human-readable reset reason
  const char* getResetReasonString(esp_reset_reason_t reason) const;
  const char* getRtcResetReasonString(RESET_REASON reason) const;

 private:
  CrashHandler() = default;
  ~CrashHandler() = default;
  CrashHandler(const CrashHandler&) = delete;
  CrashHandler& operator=(const CrashHandler&) = delete;

  // RTC memory storage (survives deep sleep and resets)
  static CrashInfo crash_info_;  // RTC_DATA_ATTR on the definition only

  bool initialized_ = false;

  // Calculate checksum
  uint16_t calculateChecksum(const CrashInfo& info) const;
  bool validateChecksum(const CrashInfo& info) const;
};

// Macro to record function entry (lightweight breadcrumb)
#ifdef DEBUG
#define CRASH_BREADCRUMB(func) CrashHandler::getInstance().recordFunction(func)
#else
#define CRASH_BREADCRUMB(func) ((void)0)
#endif
