#pragma once
// Crash forensics that survive a reset.
//
// Two pieces of RTC memory (kept across resets, lost on power loss):
//
//  * a breadcrumb naming the operation in flight, so a boot that dies mid-op
//    can say which one on the next boot rather than just "panic";
//  * a sentinel held only while an SD mount is in flight. If a boot starts and
//    finds it still set, the previous boot died inside the mount -- so the card
//    gets quarantined and can never boot-loop the controller.
//
// The breadcrumb macros stay macros so the call sites read as one word at the
// point of risk. They were the difference between diagnosing the 2026-08-05
// crash loop and guessing at it.

#include <Arduino.h>
#include <stdint.h>

#include "esp_system.h"

namespace crashlog {

inline constexpr uint32_t kSdSentinelMagic = 0x5DDEAD01;

// RTC_DATA_ATTR lives on the definitions in crashlog.cpp, not here: repeating
// it on the declaration makes gcc assign a second .rtc.data section and warn
// that it is ignoring one of them.
/** Set only while an SD mount is in flight; read back on the next boot. */
extern uint32_t rtc_sd_sentinel;

/** Name of the operation in flight when a boot dies. */
extern char rtc_crumb[16];

const char* reset_reason_str(esp_reset_reason_t r);

}  // namespace crashlog

#define CRUMB(x) snprintf(crashlog::rtc_crumb, sizeof(crashlog::rtc_crumb), "%s", x)
#define CRUMB_CLEAR() (crashlog::rtc_crumb[0] = 0)
