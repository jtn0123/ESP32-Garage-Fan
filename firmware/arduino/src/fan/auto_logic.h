#pragma once
// Copyright 2026 Justin
//
// Auto-mode decision for the garage fan: a hold-at-max thermostat with
// hysteresis. While the garage is meaningfully hotter than outdoors the fan
// runs flat-out at the user's max — partial venting wastes the temperature
// difference. Only once inside and outside are nearly equalized does it drop
// to the user's low speed (0 = off). Two thresholds (engage / release) keep
// it from flip-flopping right at the boundary.
//
// Pure header (no Arduino includes) so the native test env compiles this
// exact logic -- see boot_health.h for the precedent.

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "system/fixed_fmt.h"

// Minimum-run dwell, applied to a latch's RELEASE edge only. Both latches
// (thermostat, gas) are bang-bang controllers whose actuator changes the very
// signal they watch, so with a band narrower than the fan's own effect they
// limit-cycle (the 2026-08-16 tape: 0<->6 every ~10 min for three hours).
// Engaging stays instant -- heat and bad air get an immediate response; only
// "give up early" waits. 15 min was picked by simulating the 08-16 physics:
// 5 min is inside the natural burst length and changes nothing, 15 min cuts
// the worst-case churn to ~2 cycles/hour biased toward running.
//
// `run_ticks` is the caller-owned count of ticks spent engaged. Returns the
// latch value to honor: `want` unless it is a premature release.
inline bool latch_min_run(bool was, bool want, uint16_t* run_ticks, uint16_t min_ticks) {
  if (was && !want && *run_ticks < min_ticks)
    want = true;  // dwell not served: keep running
  if (!want)
    *run_ticks = 0;
  else if (!was)
    *run_ticks = 1;  // fresh engage: this tick is the first served
  else if (*run_ticks < UINT16_MAX)
    (*run_ticks)++;
  return want;
}

struct FanAutoCfg {
  int min_speed;      // rest speed once equalized (0 = off), user-set
  int max_speed;      // user's ceiling, held while venting pays off
  float on_delta_c;   // engage max when inside-outside >= this
  float off_delta_c;  // release to min when inside-outside <= this
};

// 2.5 F engage / 1.5 F release, expressed in C.
inline constexpr FanAutoCfg kFanAutoDefaults{0, 9, 2.5f * 5 / 9, 1.5f * 5 / 9};

// One tick of the controller. Returns the speed to command this tick.
// - `high` is the hysteresis latch, owned by the caller across ticks.
// - Missing/stale data (NaN) holds speed AND latch: never guess. The dwell
//   counter freezes too -- blind time neither serves nor resets the dwell.
// - Between the thresholds the latch holds its last state.
// - Moves at most ONE step toward the target per tick (gentle ramp, no hunt).
// - `run_ticks`/`min_run_ticks`: minimum-run dwell on the release edge (see
//   latch_min_run). Callers that pass no counter get the undwelled latch.
inline int fan_auto_decide(float inside_c, float outside_c, int prev_speed, bool* high,
                           const FanAutoCfg& cfg, uint16_t* run_ticks = nullptr,
                           uint16_t min_run_ticks = 0) {
  if (std::isnan(inside_c) || std::isnan(outside_c))
    return prev_speed;
  const float delta = inside_c - outside_c;
  bool want = *high;
  if (delta >= cfg.on_delta_c) {
    want = true;
  } else if (delta <= cfg.off_delta_c) {
    want = false;
  }
  if (run_ticks)
    want = latch_min_run(*high, want, run_ticks, min_run_ticks);
  *high = want;
  const int target = *high ? cfg.max_speed : cfg.min_speed;
  if (target == prev_speed)
    return prev_speed;
  return prev_speed + (target > prev_speed ? 1 : -1);
}

// Gas boost: the SGP41's VOC index forces a minimum speed while the air is
// bad, layered UNDER the thermostat -- it can raise the auto decision, never
// lower it. Sensirion's index is 100 = this sensor's own 24 h average, so the
// thresholds are relative badness, not an absolute concentration.
struct FanGasCfg {
  bool enabled;
  int boost_speed;  // the floor while latched
  int on_index;     // latch when VOC index >= this
  int off_index;    // release when VOC index <= this (hysteresis gap)
};

// How far below the engage index the release sits. 100, not the original 50.
//
// A 50-point gap put the release ABOVE the level the fan settles the air at,
// so the boost crossed it on the way down while it was still winning, quit,
// and the air rebounded over the engage threshold within five minutes. The
// 2026-08-23 tape: eight engage/release pairs, every one ending at exactly
// the 15-minute dwell rather than at clean air, releasing at index 121-191
// and re-engaging 8-25 minutes later. Fitted over 2106 five-minute samples of
// the device's own history, the air settles toward index 84 while the fan
// blows and climbs toward 212 while it rests -- so 150 is reachable and 200
// was never a resting point, just a number the decay passed through.
//
// The cost is deliberate: runs get longer. Of those eight cycles, three would
// still have released inside the dwell, and five would have run on past it to
// reach 150. Fewer, deeper cycles is the trade.
inline constexpr int kGasReleaseGap = 100;

// 250 engage / 150 release: comfortably past normal drift (the index recenters
// on 100), and far enough apart that the fan reaches the release point by
// cleaning the air rather than by passing through it.
inline constexpr FanGasCfg kFanGasDefaults{true, 6, 250, 250 - kGasReleaseGap};

// The floor to enforce this tick, 0 when none. `gas_high` is the hysteresis
// latch, owned by the caller across ticks.
// - index <= 0 means warming up (0) or no sensor (-1): the latch CLEARS,
//   dwell or no dwell. Holding a boost on a sensor that stopped answering
//   would pin the fan at boost speed forever with nothing left to release
//   it; the thermostat still governs, so losing the sensor degrades to plain
//   auto, not to noise.
// - `run_ticks`/`min_run_ticks`: minimum-run dwell on the release edge (see
//   latch_min_run). Callers that pass no counter get the undwelled latch.
inline int fan_gas_floor(int voc_index, bool* gas_high, const FanGasCfg& cfg,
                         uint16_t* run_ticks = nullptr, uint16_t min_run_ticks = 0) {
  if (!cfg.enabled || voc_index <= 0) {
    *gas_high = false;
    if (run_ticks)
      *run_ticks = 0;
    return 0;
  }
  bool want = *gas_high;
  if (voc_index >= cfg.on_index)
    want = true;
  else if (voc_index <= cfg.off_index)
    want = false;
  if (run_ticks)
    want = latch_min_run(*gas_high, want, run_ticks, min_run_ticks);
  *gas_high = want;
  return *gas_high ? cfg.boost_speed : 0;
}

// ------------------------------------------------------------- telemetry
//
// The tape records WHAT the thermostat did (`fan speed=9 src=local`) and has
// never recorded WHY. Reconstructing a decision afterwards meant pairing the
// 5-minute climate sample against thresholds by hand, and the dwell counter --
// the thing that decides whether a release is honoured at all -- was not
// visible anywhere. This renders one periodic line carrying the whole
// decision: both temperatures, the differential the latch actually compares,
// the latch, how much of the minimum run has been served, and the target.
//
// The recorder truncates past 80 bytes and it truncates the TAIL, so the
// worst case is pinned by the native test rather than hoped for.

/**
 * The periodic auto-mode line, e.g.
 *   "in=81.0 out=71.6 d=+9.4 latch=on dwell=12/30 tgt=10 gas=off"
 *
 * Temperatures in F because the thresholds the user sets are in F; `d` is
 * inside-minus-outside, the quantity the hysteresis actually compares.
 * Returns the length written (never past `cap`).
 */
inline int fan_auto_log_line(char* out, size_t cap, float inside_c, float outside_c, bool high,
                             uint16_t run_ticks, uint16_t min_ticks, int target, bool gas_high) {
  if (!out || cap == 0)
    return 0;
  // Every field is rendered from a CLAMPED INTEGER (system/fixed_fmt.h), so
  // the compiler can prove the whole line fits and -Wformat-truncation checks
  // it at every build rather than the tape losing its tail at 3 a.m.
  char in_f[fixedfmt::kCap], out_f[fixedfmt::kCap], d_f[fixedfmt::kCap];
  fixedfmt::write(in_f, fixedfmt::tenths_f(inside_c));
  fixedfmt::write(out_f, fixedfmt::tenths_f(outside_c));
  const bool blind = std::isnan(inside_c) || std::isnan(outside_c);
  fixedfmt::write_signed(
      d_f, blind ? fixedfmt::kAbsent : fixedfmt::tenths((inside_c - outside_c) * 9 / 5));
  return snprintf(out, cap, "in=%s out=%s d=%s latch=%s dwell=%d/%d tgt=%d gas=%s", in_f, out_f,
                  d_f, high ? "on" : "off", fixedfmt::count999(run_ticks),
                  fixedfmt::count999(min_ticks), fixedfmt::small(target), gas_high ? "ON" : "off");
}

// Merge the gas floor into the thermostat's decision, preserving the one-step
// ramp: below the floor the speed climbs one step per tick; at or above it the
// thermostat may not pull below it.
inline int fan_apply_gas_floor(int next, int prev_speed, int floor_speed) {
  if (floor_speed <= 0 || next >= floor_speed)
    return next;
  return prev_speed < floor_speed ? prev_speed + 1 : floor_speed;
}
