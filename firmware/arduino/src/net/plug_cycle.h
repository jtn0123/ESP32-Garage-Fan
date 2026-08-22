#pragma once
// Copyright 2026 Justin
//
// The fan-cycling profile: is the fan switching itself on and off while the
// controller holds one speed?
//
// Why this exists. On the night of 2026-08-20 the firmware commanded speed 10
// for nine hours straight (two reboots, zero speed changes on the tape) and
// the fan spent the night stopping and restarting every few seconds: the
// plug read ~4 W (stopped) most of the time with bursts at ~45 W -- the
// speed-12 level, not speed 10's 30 W. Nothing on the device said so. The
// plug verdict (net/plug.cpp) flapped DISAGREE/agree 87 times, which reads
// as "the meter is noisy", and the 5-minute history snapshot aliased the
// whole thing into a jittery line. The control line is fire-and-forget, so
// the meter is the only witness, and this is the profile that turns its
// testimony into one sentence: "the fan is cycling, N flips in 10 min, peak
// X W, trough Y W, while speed S was held".
//
// Pure header (no Arduino includes): the native_plug_cycle env replays
// synthetic meters and fan schedules through this exact code.
//
// Method. Every poll (15 s) the reading is classed against the commanded
// speed's baseline: RUN at >= 65 % of it, STOP at <= 35 %, anything between
// is the meter averaging across an edge and votes for neither. A class that
// differs from the last decided class, once it has held for two polls, is a
// flip. Flips are remembered per poll in a 64-bit shift register; CYCLING is
// declared when the last 40 polls (10 min) hold at least 4 flips, and ends
// once 60 polls (15 min) pass without one -- longer than the onset window so
// a lull inside a bad night does not end and restart the episode every
// quarter hour. A speed change resets everything: the first minute after a change
// is spin-up plus meter lag, and the plug module already refuses to judge
// it (kSettleMs) -- `settled` carries that refusal in here.
//
// Why 65/35 and not the verdict's 50 % band: the band says "about right"; the
// classes need to say "definitely running" and "definitely stopped" with a
// gap the meter's averaging can fall into without being counted. Why the
// 7 W floor: below speed 3 the baseline (3.7 / 4.8 W) sits within a couple
// of watts of the fan's own idle electronics, and a meter cannot tell a
// stopped speed-1 fan from a running one. No profile beats a wrong profile.

#include <stdint.h>

namespace plug {

struct CycleCfg {
  float run_frac;         // >= this fraction of the baseline = running
  float stop_frac;        // <= this fraction of the baseline = stopped
  float min_expect_w;     // below this baseline the classes are not separable
  uint8_t confirm_polls;  // consecutive polls a class must hold to count
  uint8_t window;         // polls the onset count looks back over (<= 64)
  uint8_t onset_flips;    // flips inside `window` that declare CYCLING
  uint8_t quiet_polls;    // flip-free polls that end an episode (<= 64)
};

// 15 s polls: confirm 2 = 30 s, window 40 = 10 min, quiet 60 = 15 min.
// Onset at 4 flips = two full stop/start rounds, which is the smallest
// pattern that is a cycle and not a hiccup (one stop and restart is 2).
// confirm_polls 2 is what keeps a single-poll meter blip (one 0 W reading
// between two good ones) from counting as a stop AND a restart.
inline constexpr CycleCfg kCycleDefaults{0.65f, 0.35f, 7.0f, 2, 40, 4, 60};

// trough_w before any decidable reading landed in the episode.
inline constexpr float kNoTrough = 1e9f;

/**
 * Which band one reading falls in: RUN (+1), STOP (-1), or undecided (0) --
 * the meter averaging across an edge, no reading at all, or a speed whose
 * baseline is too low to separate a stopped fan from a running one.
 *
 * The ONE definition of the bands: the detector decides with it and the
 * telemetry window counts with it, so "28 of 36 polls looked stopped" and
 * "this is cycling" can never come from two different rules.
 */
inline int8_t classify(float w, float expect_w, const CycleCfg& cfg = kCycleDefaults) {
  if (w != w || expect_w != expect_w || expect_w < cfg.min_expect_w)  // x != x: NaN
    return 0;
  if (w >= expect_w * cfg.run_frac)
    return 1;
  if (w <= expect_w * cfg.stop_frac)
    return -1;
  return 0;
}

// What one poll decided.
enum class CycleEvent : int8_t { kNone = 0, kOnset = 1, kEnded = -1 };

struct CycleDetector {
  // --- the profile ------------------------------------------------------
  uint64_t flip_bits = 0;  // bit i set = a flip was counted i polls ago
  int8_t last_class = 0;   // +1 run, -1 stop, 0 nothing decided yet
  int8_t pending = 0;      // class seen on the latest poll(s), not yet confirmed
  uint8_t pending_n = 0;   // consecutive polls `pending` has held
  int speed_seen = -99;
  bool cycling = false;

  // --- the testimony (valid from onset until the next onset) -------------
  uint16_t polls = 0;        // polls since onset
  uint16_t run_polls = 0;    // of those, classed RUN
  uint16_t stop_polls = 0;   // of those, classed STOP
  uint16_t flips_total = 0;  // flips since onset (the window only sees 40)
  float peak_w = 0;          // loudest reading since onset
  float trough_w = 0;        // quietest reading since onset (kNoTrough = none yet)
  uint8_t bucket_flips = 0;  // flips since take_bucket_flips(), for history

  /** Flips inside the last `polls` polls. */
  uint8_t flips_in(uint8_t polls) const {
    const uint64_t mask = polls >= 64 ? ~0ULL : ((1ULL << polls) - 1);
    uint64_t v = flip_bits & mask;
    uint8_t n = 0;
    while (v) {
      v &= v - 1;
      n++;
    }
    return n;
  }

  /** Flips inside the onset window right now -- the number the console shows. */
  uint8_t flips_in_window(const CycleCfg& cfg = kCycleDefaults) const {
    return flips_in(cfg.window);
  }

  /** Flips counted since the last call; the 5-minute history row reads this. */
  uint8_t take_bucket_flips() {
    const uint8_t n = bucket_flips;
    bucket_flips = 0;
    return n;
  }

  /**
   * One meter poll.
   *  speed     commanded speed (anything; a change resets the profile)
   *  expect_w  the baseline draw for that speed (NaN = no table entry)
   *  w         the reading (NaN = no reading this poll)
   *  settled   the speed has been steady long enough for the meter to have
   *            caught up -- the caller's kSettleMs gate
   * Returns the edge this poll produced, if any.
   */
  CycleEvent poll(int speed, float expect_w, float w, bool settled,
                  const CycleCfg& cfg = kCycleDefaults) {
    if (speed != speed_seen) {
      speed_seen = speed;
      flip_bits = 0;
      last_class = 0;
      pending = 0;
      pending_n = 0;
      if (cycling) {
        // A speed change ends the episode: the profile is "cycling AT a
        // speed", and the next speed gets a fresh look.
        cycling = false;
        return CycleEvent::kEnded;
      }
      return CycleEvent::kNone;
    }
    // The register ages by one poll whether or not this poll can decide
    // anything, so "10 minutes" stays wall-clock, not "40 decided polls".
    flip_bits <<= 1;
    // The bands live in classify() so the telemetry window counts polls by
    // exactly the rule the detector decides by.
    const bool decidable = settled;
    const int8_t cls = decidable ? classify(w, expect_w, cfg) : 0;
    if (decidable) {
      if (cls != 0) {
        // Confirm a class across confirm_polls before it can flip anything:
        // a lone blip in either direction is the meter, not the fan.
        if (cls == pending) {
          if (pending_n < 255)
            pending_n++;
        } else {
          pending = cls;
          pending_n = 1;
        }
        if (pending_n >= cfg.confirm_polls && cls != last_class) {
          if (last_class != 0) {
            flip_bits |= 1;
            if (bucket_flips < 255)
              bucket_flips++;
            if (cycling && flips_total < UINT16_MAX)
              flips_total++;
          }
          last_class = cls;
        }
      }
    }
    const uint8_t n = flips_in(cfg.window);
    CycleEvent ev = CycleEvent::kNone;
    if (!cycling && n >= cfg.onset_flips) {
      cycling = true;
      polls = run_polls = stop_polls = 0;
      flips_total = n;
      peak_w = 0;
      trough_w = kNoTrough;
      ev = CycleEvent::kOnset;
    }
    if (cycling) {
      // The onset poll counts too: its reading is part of the testimony.
      polls++;
      if (cls > 0)
        run_polls++;
      else if (cls < 0)
        stop_polls++;
      if (decidable) {
        if (w > peak_w)
          peak_w = w;
        if (w < trough_w)
          trough_w = w;
      }
      if (flips_in(cfg.quiet_polls) == 0) {
        cycling = false;
        ev = CycleEvent::kEnded;
      }
    }
    return ev;
  }
};

}  // namespace plug
