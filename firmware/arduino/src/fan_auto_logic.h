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
