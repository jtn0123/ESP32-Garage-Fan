#pragma once
// A ring of breadcrumbs in RTC memory: the last N things the firmware was
// doing, with timestamps, readable on the next boot.
//
// This is the crash evidence that actually works on THIS board. Core dumps do
// not: the deployed Feather runs tinyuf2-partitions-4MB.csv, which has no
// coredump partition (see system/coredump.h for how that was learned the hard
// way). RTC memory needs no partition, no flash write, and no driver -- it
// survives esp_restart and a panic reboot, and it is the one thing still
// standing after a hang.
//
// Why a ring rather than the single crumb crashlog already had: the single
// slot answers "what was in flight" only if the crash happens INSIDE a marked
// operation. The unexplained panic on 2026-08-11 happened 75-100 s AFTER a
// purge returned, with the crumb already cleared -- so it recorded nothing at
// all. A ring keeps the approach as well as the arrival: the last N steps
// before the fall, which is what "it died somewhere after the purge" needs.
//
// RTC caveat, unchanged from crashlog: this survives resets, NOT power loss.
// A power-cycled board starts with an empty ring, which is also why the
// rollback counter cannot be relied on after someone pulls the plug.

#include <stdint.h>
#include <string.h>

namespace crumb_ring {

constexpr uint8_t kSlots = 16;
constexpr uint8_t kTagLen = 14;

struct Entry {
  uint32_t ms;        // millis() when it was dropped
  char tag[kTagLen];  // short, fixed: RTC memory is scarce
};

/**
 * The ring itself. Defined in the .cpp with RTC_DATA_ATTR; declared here
 * without it, because repeating the attribute makes gcc emit a second
 * .rtc.data section and warn that it is ignoring one (the same trap
 * crashlog.h documents).
 */
extern Entry rtc_entries[kSlots];
extern uint32_t rtc_next;   // write cursor
extern uint32_t rtc_magic;  // validity marker; RTC is garbage after power-on

constexpr uint32_t kMagic = 0xC0FFEE11;

/** Drop a breadcrumb. Cheap enough to call on every meaningful step. */
void drop(const char* tag, uint32_t now_ms);

/** Start a fresh trail (called once at boot, after the previous one is read). */
void reset();

/** Was the stored trail written by a previous run rather than power-on noise? */
bool valid();

/**
 * Oldest-to-newest into `out` as "tag@ms tag@ms ...".
 *
 * One line, because it goes on the flight recorder next to the boot verdict
 * and the point is to read the approach to the crash at a glance.
 */
void render(char* out, size_t cap);

}  // namespace crumb_ring
