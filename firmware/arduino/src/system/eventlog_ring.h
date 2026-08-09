#pragma once
// Fixed-footprint line ring behind system/eventlog, kept Arduino-free so the
// native_eventlog_ring env can test wrap, truncation and the flush ledger on
// the host. The device instance lives in eventlog.cpp; /api/events reads the
// ring, and the flush ledger tracks which lines still owe an SD write so
// events logged before the card mounts (or while it is missing) are not lost
// to the file the moment it appears.

#include <stdint.h>

#include <cstring>

namespace eventlog {

template <uint16_t kMaxLines, uint16_t kLineLen>
struct LineRing {
  char buf[kMaxLines][kLineLen];
  uint16_t head = 0;     // next slot to write
  uint16_t n = 0;        // retained lines
  uint32_t total = 0;    // lifetime appends
  uint32_t flushed = 0;  // lifetime lines that reached SD (or were lost)
  uint32_t lost = 0;     // overwritten before they could be flushed

  void append(const char* line) {
    if (n == kMaxLines && total - flushed >= kMaxLines) {
      // About to overwrite a line that never reached SD: advance the ledger
      // past it so the flush pointer cannot drift behind the ring forever.
      flushed++;
      lost++;
    }
    strncpy(buf[head], line, kLineLen - 1);
    buf[head][kLineLen - 1] = '\0';
    head = static_cast<uint16_t>((head + 1) % kMaxLines);
    if (n < kMaxLines)
      n++;
    total++;
  }

  /** Retained line i, where 0 is the OLDEST still in the ring. */
  const char* line(uint16_t i) const {
    const uint16_t start = static_cast<uint16_t>((head + kMaxLines - n) % kMaxLines);
    return buf[(start + i) % kMaxLines];
  }

  /** Lines still owing an SD write (always <= n by the append guard). */
  uint16_t unflushed() const { return static_cast<uint16_t>(total - flushed); }

  /** Index (for line()) of the oldest unflushed line. */
  uint16_t first_unflushed() const { return static_cast<uint16_t>(n - unflushed()); }

  void mark_flushed(uint16_t count) { flushed += count; }
};

}  // namespace eventlog
