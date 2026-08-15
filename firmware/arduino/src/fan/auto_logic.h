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
// - Missing/stale data (NaN) holds speed AND latch: never guess.
// - Between the thresholds the latch holds its last state.
// - Moves at most ONE step toward the target per tick (gentle ramp, no hunt).
inline int fan_auto_decide(float inside_c, float outside_c, int prev_speed, bool* high,
                           const FanAutoCfg& cfg) {
  if (std::isnan(inside_c) || std::isnan(outside_c))
    return prev_speed;
  const float delta = inside_c - outside_c;
  if (delta >= cfg.on_delta_c) {
    *high = true;
  } else if (delta <= cfg.off_delta_c) {
    *high = false;
  }
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

// 250/200: comfortably past normal drift (the index recenters on 100), wide
// enough apart that cooking-adjacent wobble does not flap the fan.
inline constexpr FanGasCfg kFanGasDefaults{true, 6, 250, 200};

// The floor to enforce this tick, 0 when none. `gas_high` is the hysteresis
// latch, owned by the caller across ticks.
// - index <= 0 means warming up (0) or no sensor (-1): the latch CLEARS.
//   Holding a boost on a sensor that stopped answering would pin the fan at
//   boost speed forever with nothing left to release it; the thermostat still
//   governs, so losing the sensor degrades to plain auto, not to noise.
inline int fan_gas_floor(int voc_index, bool* gas_high, const FanGasCfg& cfg) {
  if (!cfg.enabled || voc_index <= 0) {
    *gas_high = false;
    return 0;
  }
  if (voc_index >= cfg.on_index)
    *gas_high = true;
  else if (voc_index <= cfg.off_index)
    *gas_high = false;
  return *gas_high ? cfg.boost_speed : 0;
}

// Merge the gas floor into the thermostat's decision, preserving the one-step
// ramp: below the floor the speed climbs one step per tick; at or above it the
// thermostat may not pull below it.
inline int fan_apply_gas_floor(int next, int prev_speed, int floor_speed) {
  if (floor_speed <= 0 || next >= floor_speed)
    return next;
  return prev_speed < floor_speed ? prev_speed + 1 : floor_speed;
}
