// See crumb_ring.h.
#include "system/crumb_ring.h"

#include <Arduino.h>

#include <cstdio>

namespace crumb_ring {

// RTC_DATA_ATTR belongs on the definitions only; see the note in the header.
RTC_DATA_ATTR Entry rtc_entries[kSlots];
RTC_DATA_ATTR uint32_t rtc_next = 0;
RTC_DATA_ATTR uint32_t rtc_magic = 0;

bool valid() { return rtc_magic == kMagic && rtc_next <= 0xFFFF; }

void reset() {
  rtc_magic = kMagic;
  rtc_next = 0;
  for (auto& e : rtc_entries) {
    e.ms = 0;
    e.tag[0] = '\0';
  }
}

void drop(const char* tag, uint32_t now_ms) {
  if (!tag)
    return;
  if (rtc_magic != kMagic)
    reset();  // first use, or RTC came up as noise after a power-on
  Entry& e = rtc_entries[rtc_next % kSlots];
  e.ms = now_ms;
  snprintf(e.tag, kTagLen, "%s", tag);
  rtc_next++;
}

void render(char* out, size_t cap) {
  if (!out || cap == 0)
    return;
  out[0] = '\0';
  if (!valid())
    return;
  // Oldest first. Once the ring has wrapped, the oldest live entry is the one
  // the cursor is about to overwrite.
  const uint32_t total = rtc_next;
  const uint32_t count = total < kSlots ? total : kSlots;
  const uint32_t start = total < kSlots ? 0 : total - kSlots;
  size_t n = 0;
  for (uint32_t i = 0; i < count && n + 1 < cap; i++) {
    const Entry& e = rtc_entries[(start + i) % kSlots];
    if (e.tag[0] == '\0')
      continue;
    const int w = snprintf(out + n, cap - n, n ? " %s@%lu" : "%s@%lu", e.tag, (unsigned long)e.ms);
    if (w < 0 || static_cast<size_t>(w) >= cap - n)
      break;
    n += static_cast<size_t>(w);
  }
}

}  // namespace crumb_ring
