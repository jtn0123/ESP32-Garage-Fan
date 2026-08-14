#pragma once
// A ring of breadcrumbs: the last N things the firmware was doing, with
// timestamps, readable on the next boot.
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
// operation. The unexplained panic happened long AFTER a purge returned, with
// the crumb already cleared -- so it recorded nothing at all. A ring keeps the
// approach as well as the arrival.
//
// The state and all its logic live in `Ring`, a plain struct with no Arduino
// or RTC dependency, so the host tests can drive it directly
// (native_crumb_ring). The firmware keeps one instance of it in RTC memory.
// That split exists because the first version of this file shipped a bug --
// validity was gated on a 16-bit cursor limit that the loop blew past in under
// a minute, silently disabling the ring on exactly the boot it was built for --
// and nothing could have caught it, because nothing could run it.
//
// RTC caveat: this survives resets, NOT power loss. A power-cycled board starts
// with an empty ring, which is also why the rollback counter cannot be relied
// on after someone pulls the plug.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace crumb_ring {

constexpr uint8_t kSlots = 16;
constexpr uint8_t kTagLen = 14;
constexpr uint32_t kMagic = 0xC0FFEE11;

struct Entry {
  uint32_t ms;        // millis() when it was dropped
  char tag[kTagLen];  // short, fixed: RTC memory is scarce
};

/** The ring's state and behaviour, free of any platform dependency. */
struct Ring {
  Entry entries[kSlots];
  // Slot index and live count, NOT a running total. A total overflows: the
  // loop drops several crumbs per iteration, so a uint32_t counter wraps after
  // roughly three weeks of uptime -- and at the wrap `count` computed from it
  // fell to zero and the trail rendered empty, on a device meant to run for
  // months. These two never overflow because neither exceeds kSlots.
  uint8_t next;    // where the next drop goes
  uint8_t count;   // live entries, saturating at kSlots
  uint32_t magic;  // validity marker; RTC is garbage after power-on

  /**
   * Was this written by a previous run rather than power-on noise?
   *
   * Magic plus the two INDEX bounds. An earlier version also required
   * next <= 0xFFFF; the loop drops several crumbs per iteration, so that limit
   * was passed in well under a minute and the ring reported itself invalid for
   * the rest of the run -- rendering nothing on precisely the boot after a
   * long-running fault. That was a bound on a COUNTER and it was wrong.
   *
   * These two are bounds on INDEXES, which is a different thing: next is kept
   * modulo kSlots and count saturates at kSlots, so no amount of running can
   * push either out of range. Only corruption can -- and RTC memory that comes
   * up with the magic intact but a garbage next is exactly what would make
   * drop() write past `entries`. The module that exists to explain crashes must
   * not be the one causing them.
   */
  bool valid() const { return magic == kMagic && next < kSlots && count <= kSlots; }

  void reset() {
    magic = kMagic;
    next = 0;
    count = 0;
    for (auto& e : entries) {
      e.ms = 0;
      e.tag[0] = '\0';
    }
  }

  /** Drop a breadcrumb. Cheap enough to call on every meaningful step. */
  void drop(const char* tag, uint32_t now_ms) {
    if (!tag || !*tag)
      return;
    if (!valid())
      reset();  // first use, or RTC came up as noise after a power-on
    Entry& e = entries[next];
    e.ms = now_ms;
    snprintf(e.tag, kTagLen, "%s", tag);
    next = static_cast<uint8_t>((next + 1) % kSlots);
    if (count < kSlots)
      count++;
  }

  /**
   * NEWEST-to-oldest into `out` as "tag@ms tag@ms ...".
   *
   * One line, because it goes on the flight recorder next to the boot verdict
   * and the point is to read the approach to the crash at a glance. Newest
   * first because something always clips this line -- the eventlog caps rows
   * at ~104 chars -- and when it does, the steps NEAREST the death must be
   * the ones that survive. The first deployed trail proved it the other way
   * round: oldest-first, clipped mid-way, newest steps lost (2026-08-13).
   */
  void render(char* out, size_t cap) const {
    if (!out || cap == 0)
      return;
    out[0] = '\0';
    if (!valid())
      return;
    // The newest live entry is the one just before `next`.
    size_t n = 0;
    for (uint8_t i = 0; i < count && n + 1 < cap; i++) {
      const Entry& e = entries[(next + kSlots - 1 - i) % kSlots];
      if (e.tag[0] == '\0')
        continue;
      const int w =
          snprintf(out + n, cap - n, n ? " %s@%lu" : "%s@%lu", e.tag, (unsigned long)e.ms);
      if (w < 0 || static_cast<size_t>(w) >= cap - n) {
        // snprintf already wrote a truncated prefix. Cut it back off: half a
        // "tag@ms" pair reads as a real step that never happened, and this line
        // is evidence at a crash site.
        out[n] = '\0';
        break;
      }
      n += static_cast<size_t>(w);
    }
  }
};

// The firmware's single instance, in RTC memory. Declared without
// RTC_DATA_ATTR here on purpose: repeating the attribute makes gcc emit a
// second .rtc.data section and warn that it is ignoring one (the same trap
// crashlog.h documents).
extern Ring rtc_ring;

void drop(const char* tag, uint32_t now_ms);
void reset();
bool valid();
void render(char* out, size_t cap);

}  // namespace crumb_ring
