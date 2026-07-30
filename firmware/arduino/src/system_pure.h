#pragma once
// Copyright 2024 Justin
//
// The parts of system_manager that are pure functions of their arguments:
// reset-reason classification and CRC32.
//
// They live in their own header so the native unit tests can include and
// exercise the real code. system_manager.cpp itself cannot be compiled off
// target — it pulls in esp_heap_caps, esp_sleep, Preferences and the Logger — and
// the alternative (a hand-copied duplicate inside the test) is what let
// test_power's device-mode logic silently drift from the implementation.
//
// Only esp_system.h is needed, for esp_reset_reason_t; the native test supplies a
// shim providing that enum.

#include <esp_system.h>
#include <cstddef>
#include <cstdint>

inline const char* reset_reason_to_string(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:
      return "UNKNOWN";
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_EXT:
      return "EXTERNAL";
    case ESP_RST_SW:
      return "SOFTWARE";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "INT_WATCHDOG";
    case ESP_RST_TASK_WDT:
      return "TASK_WATCHDOG";
    case ESP_RST_WDT:
      return "WATCHDOG";
    case ESP_RST_DEEPSLEEP:
      return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_SDIO:
      return "SDIO";
    default:
      return "UNKNOWN";
  }
}

// A reset the firmware should treat as a fault worth reporting, as opposed to a
// normal power-on or a scheduled wake from deep sleep.
inline bool reset_reason_indicates_crash(esp_reset_reason_t reason) {
  return (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
          reason == ESP_RST_WDT || reason == ESP_RST_BROWNOUT);
}

// Bitwise CRC32 (polynomial 0xEDB88320), used to detect changes in short status
// strings before spending an e-ink refresh on them. Not table-driven: the inputs
// are tens of bytes, so the loop costs less than the table would in flash.
inline uint32_t crc32_bytes(const uint8_t* data, size_t len) {
  if (!data)
    return 0;

  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= static_cast<uint32_t>(data[i]);
    for (int k = 0; k < 8; ++k) {
      uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}
