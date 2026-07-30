#pragma once
// Copyright 2024 Justin
//
// Dev-mode state machine as a pure value type.
//
// power.cpp cannot be compiled on the native platform (Serial, Wire, the
// Adafruit fuel-gauge drivers), so test_power used to keep a hand-typed copy of
// this logic. The copy drifted: it kept asserting correctly while the real
// is_dev_mode() carried a sentinel bug where a mode entered at millis() == 0
// never expired, because 0 doubled as "unset". Nobody saw it, since CI only ran
// test_rounding.
//
// Keeping the state in a struct with the clock injected means production and the
// unit test exercise the same code, and a future divergence is impossible rather
// than merely discouraged.

#include <cstdint>
#include <cstring>

// Dev mode reverts to production automatically after this long, so a device left
// in dev mode by an MQTT command cannot stay there indefinitely.
#ifndef DEV_MODE_TIMEOUT_MS
#define DEV_MODE_TIMEOUT_MS 3600000UL  // 1 hour
#endif

struct DevModeState {
  bool active = false;
  uint32_t started_ms = 0;

  // `active` is the authoritative flag; `started_ms` is only a timestamp. Do not
  // reintroduce a "started_ms == 0 means inactive" shortcut: millis() really does
  // return 0 for the first millisecond after boot.
  void set(const char* mode, uint32_t now_ms) {
    if (mode && (std::strcmp(mode, "dev") == 0 || std::strcmp(mode, "development") == 0)) {
      active = true;
      started_ms = now_ms;
    } else {
      active = false;
      started_ms = 0;
    }
  }

  // True while dev mode is still in force. Expires (and clears) the state once
  // the timeout has elapsed. Unsigned subtraction handles the ~49-day millis()
  // rollover on its own.
  bool is_active(uint32_t now_ms) {
    if (!active)
      return false;
    if ((now_ms - started_ms) >= DEV_MODE_TIMEOUT_MS) {
      active = false;
      started_ms = 0;
      return false;
    }
    return true;
  }

  // Seconds left before auto-expiry, 0 when not active.
  uint32_t remaining_sec(uint32_t now_ms) {
    if (!is_active(now_ms))
      return 0;
    uint32_t elapsed = now_ms - started_ms;
    if (elapsed >= DEV_MODE_TIMEOUT_MS)
      return 0;
    return (DEV_MODE_TIMEOUT_MS - elapsed) / 1000;
  }
};

// Minimum accepted sleep/sample cadence. Polling the BME280 harder than this
// self-heats it and biases the temperature reading upward.
#ifndef MIN_SLEEP_INTERVAL_SEC
#define MIN_SLEEP_INTERVAL_SEC 180
#endif

inline uint32_t clamp_sleep_interval_sec(uint32_t sec) {
  return (sec < MIN_SLEEP_INTERVAL_SEC) ? MIN_SLEEP_INTERVAL_SEC : sec;
}
