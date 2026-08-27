#pragma once
// Copyright 2026 Justin
//
// What the watt meter saw over a window of polls, and how to say it in one
// line.
//
// The flight recorder keeps 24 lines in RAM (system/eventlog.cpp), so the way
// to make the tape more useful is more DATA PER LINE, never more lines. This
// header owns the accumulator behind the 5-minute `plug window ...` line and
// the raw-sample ring behind /api/plugtrace, plus the one thing the 2026-08-20
// night needed and nobody printed: the speed the MEASURED draw implies. The
// fan was pulling 45 W -- the speed-12 level -- while the controller held
// speed 10, and every line on the tape reported only the commanded number.
//
// Pure header (no Arduino includes), so native_plug_window exercises the
// arithmetic, the encoding limits and -- the part a device can only fail at
// 3 a.m. -- that the worst-case rendered line fits the recorder's 80-byte
// message budget without silent truncation.

#include <stdint.h>

#include <cmath>
#include <cstdio>

#include "system/fixed_fmt.h"

namespace plug {

/** Longest message eventlog::log renders before it truncates (char msg[80]). */
inline constexpr size_t kLogMsgCap = 80;

/**
 * The table speed whose baseline draw is nearest `w` -- "45 W is ~speed 12".
 * Returns -1 when there is no reading to place.
 */
inline int nearest_speed(const float* table, uint8_t n, float w) {
  if (!table || n == 0 || std::isnan(w))
    return -1;
  uint8_t best = 0;
  for (uint8_t i = 1; i < n; i++) {
    const float di = table[i] - w < 0 ? w - table[i] : table[i] - w;
    const float db = table[best] - w < 0 ? w - table[best] : table[best] - w;
    if (di < db)
      best = i;
  }
  return best;
}

/** One poll as the trace ring keeps it: 4 bytes, so 80 of them cost 320. */
struct Reading {
  uint16_t dw;   // watts x10, 0xFFFF = no reading this poll
  int8_t speed;  // commanded speed at the time
  int8_t cls;    // +1 running, -1 stopped, 0 undecided (see plug_cycle.h)

  static constexpr uint16_t kNoRead = 0xFFFF;

  /** Watts back out, or NaN when the poll had no reading. */
  float watts() const {
    if (dw == kNoRead)
      return 0.0f / 0.0f;
    return dw / 10.0f;
  }
};

/** Encode a reading; anything past 6553.4 W saturates rather than wrapping. */
inline Reading encode_reading(float w, int speed, int cls) {
  Reading r{Reading::kNoRead, static_cast<int8_t>(speed), static_cast<int8_t>(cls)};
  if (!std::isnan(w) && w >= 0.0f) {
    const float dw = w * 10.0f + 0.5f;
    r.dw = dw >= 65534.0f ? 65534 : static_cast<uint16_t>(dw);
  }
  return r;
}

/**
 * The last `kN` polls, oldest first once wrapped. Serves /api/plugtrace: 20
 * minutes of raw 15 s samples that cost the tape nothing, because a fan that
 * cycles faster than one line per five minutes cannot be described by lines
 * at all.
 */
template <uint8_t kN>
struct Trace {
  Reading buf[kN];
  uint8_t n = 0;
  uint8_t head = 0;  // next slot to write

  void push(const Reading& r) {
    buf[head] = r;
    head = static_cast<uint8_t>((head + 1) % kN);
    if (n < kN)
      n++;
  }

  /** Sample i, 0 = oldest retained. */
  const Reading& at(uint8_t i) const {
    const uint8_t start = n == kN ? head : 0;
    return buf[(start + i) % kN];
  }

  uint8_t size() const { return n; }
};

/**
 * Everything the meter said since the last take(), folded down.
 *
 * `changed` is the metric that measures the METER rather than the fan: the
 * Tapo's own sensor updates on its own schedule, and a window where 3 of 20
 * polls carried a new number means this device is oversampling a slow sensor
 * -- which is exactly the blind spot that lets a fast cycle read as a steady
 * mid-band draw.
 */
struct Window {
  uint16_t polls = 0;    // polls in the window, readable or not
  uint16_t run = 0;      // classed running
  uint16_t stop = 0;     // classed stopped
  uint16_t changed = 0;  // polls whose reading differed from the one before
  uint16_t missed = 0;   // polls with no reading at all
  uint16_t flips = 0;    // confirmed run/stop flips (fed in from the detector)
  float min_w = 0;
  float max_w = 0;
  float sum_w = 0;
  uint16_t n_w = 0;  // polls that contributed a reading
  float last_w = 0.0f / 0.0f;
  bool have_last = false;

  void feed(float w, int cls) {
    polls++;
    if (cls > 0)
      run++;
    else if (cls < 0)
      stop++;
    if (std::isnan(w)) {  // the meter did not answer
      missed++;
      return;
    }
    if (!n_w || w < min_w)
      min_w = w;
    if (!n_w || w > max_w)
      max_w = w;
    sum_w += w;
    n_w++;
    // Exact inequality on purpose: HA serves the same string until its own
    // sensor updates, so "differs at all" is the honest update signal.
    if (!have_last || w != last_w)
      changed++;
    last_w = w;
    have_last = true;
  }

  void add_flips(uint16_t n) { flips = static_cast<uint16_t>(flips + n); }

  float mean() const { return n_w ? sum_w / n_w : 0.0f / 0.0f; }

  /** Clear the counters; `last_w` survives so `changed` stays meaningful. */
  void reset() {
    const float keep = last_w;
    const bool had = have_last;
    *this = Window{};
    last_w = keep;
    have_last = had;
  }
};

/**
 * The 5-minute telemetry line's message, e.g.
 *   "sp10/~12 pad=84% w=4.3/12.7/45.3 on=8 off=28 new=12 miss=0 cyc=5"
 *
 * Everything the 08-20 night needed, on one line: what was commanded and what
 * the draw IMPLIES (`sp10/~12` -- held at 10, pulling the speed-12 level), what
 * the control pad was actually doing, the RANGE of the draw rather than one
 * snapshot, and how much of the window the fan spent stopped.
 *
 * Every field is clamped so the worst case still fits kLogMsgCap; the native
 * test pins that, because a line the recorder truncates loses its tail fields
 * silently and the tail is where the counts are.
 */
inline int format_window_line(char* out, size_t cap, const Window& w, int speed, int pad_pct,
                              int implied) {
  if (!out || cap == 0)
    return 0;
  if (!w.n_w) {
    // No readings at all: say that plainly rather than printing zeroes that
    // would read as a fan drawing nothing.
    return snprintf(out, cap, "sp%d pad=%d%% no meter reads in %d polls", fixedfmt::small(speed),
                    fixedfmt::count999(static_cast<uint16_t>(pad_pct < 0 ? 0 : pad_pct)),
                    fixedfmt::count999(w.polls));
  }
  char lo[fixedfmt::kCap], mid[fixedfmt::kCap], hi[fixedfmt::kCap];
  fixedfmt::write(lo, fixedfmt::tenths(w.min_w));
  fixedfmt::write(mid, fixedfmt::tenths(w.mean()));
  fixedfmt::write(hi, fixedfmt::tenths(w.max_w));
  return snprintf(out, cap, "sp%d/~%d pad=%d%% w=%s/%s/%s on=%d off=%d new=%d miss=%d cyc=%d",
                  fixedfmt::small(speed), fixedfmt::small(implied),
                  fixedfmt::count999(static_cast<uint16_t>(pad_pct < 0 ? 0 : pad_pct)), lo, mid, hi,
                  fixedfmt::count99(w.run), fixedfmt::count99(w.stop), fixedfmt::count99(w.changed),
                  fixedfmt::count99(w.missed), fixedfmt::count99(w.flips));
}

}  // namespace plug
