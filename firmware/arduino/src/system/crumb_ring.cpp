// See crumb_ring.h. This file is only the RTC-memory instance and the thin
// free functions over it; all the behaviour lives in Ring so the host can
// test it.
#include "system/crumb_ring.h"

#include <Arduino.h>

namespace crumb_ring {

// NOINIT: the DATA variant is re-initialised on every reset except deep-sleep
// wake, which zeroed the ring at boot -- 17 boots on the flight recorder and
// not one trail line (2026-08-13). The ring's magic already guards against
// the power-on garbage NOINIT leaves behind.
RTC_NOINIT_ATTR Ring rtc_ring;

bool valid() { return rtc_ring.valid(); }
void reset() { rtc_ring.reset(); }
void drop(const char* tag, uint32_t now_ms) { rtc_ring.drop(tag, now_ms); }
void render(char* out, size_t cap) { rtc_ring.render(out, cap); }

}  // namespace crumb_ring
