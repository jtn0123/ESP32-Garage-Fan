#pragma once
// Copyright 2026 Justin
//
// Auto-mode decision for the garage fan: a differential thermostat comparing
// garage temperature against outdoors. Hotter outside -> idle at min speed
// (venting would import heat); hotter inside -> ramp toward max, scaled by
// how much hotter, one step per tick so the motor ramps gently and never
// hunts.
//
// Pure header (no Arduino includes) so the native test env compiles this
// exact logic -- see boot_health.h for the precedent.

#include <cmath>

struct FanAutoCfg {
  int min_speed;     // floor while auto is on (default 1: keep air moving)
  int max_speed;     // user's ceiling (their "9"), never exceeded
  float deadband_c;  // inside-outside delta treated as "same temperature"
  float span_c;      // delta beyond deadband that maps to full max_speed
};

inline constexpr FanAutoCfg kFanAutoDefaults{1, 9, 0.3f, 4.0f};

// One tick of the controller. Returns the speed to command this tick.
// - Missing/stale data (NaN) holds the current speed: never guess.
// - Moves at most ONE step toward the target per tick (gentle ramp, no hunt).
inline int fan_auto_decide(float inside_c, float outside_c, int prev_speed,
                           const FanAutoCfg& cfg) {
  if (std::isnan(inside_c) || std::isnan(outside_c)) return prev_speed;
  const float delta = inside_c - outside_c;
  int target;
  if (delta <= cfg.deadband_c) {
    target = cfg.min_speed;
  } else {
    const float frac = (delta - cfg.deadband_c) / cfg.span_c;
    target =
        cfg.min_speed +
        static_cast<int>(std::lround(frac * (cfg.max_speed - cfg.min_speed)));
    if (target > cfg.max_speed) target = cfg.max_speed;
  }
  if (target == prev_speed) return prev_speed;
  return prev_speed + (target > prev_speed ? 1 : -1);
}
