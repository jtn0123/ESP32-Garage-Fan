#pragma once
// The only sanctioned way to restart this board.
//
// The overnight sample gaps of 2026-08-12 came from sw_reset reboots that
// nothing explained: the flight recorder held a healthy line, then a boot,
// because half the esp_restart() call sites logged their reason to Serial --
// which a deployed S2 drops -- or to nothing at all. A reboot whose reason is
// not on the tape costs a multi-hour investigation to guess at; this header
// makes the reason part of the act.
//
// Call sites: pass WHY, in the words you would want to read at 2 a.m.
#include "system/eventlog.h"

#include <Arduino.h>

#include "esp_system.h"

namespace sysreboot {

/** Log the reason, flush it to the card, then restart. Never returns. */
[[noreturn]] inline void restart(const char* reason) {
  eventlog::log("reboot", "%s", reason);
  eventlog::flush_tick();  // best effort: land the reason on SD first
  Serial.println(reason);
  Serial.flush();
  esp_restart();
}

}  // namespace sysreboot
