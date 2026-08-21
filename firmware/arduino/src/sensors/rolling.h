#pragma once
// Copyright 2026 Justin
//
// Rolling mean over the last N valid readings, so one flaky sample cannot
// yank a control decision (the 2026-08-20 hunt: single instantaneous
// readings flipping the auto-mode latches). Which signals are smoothed and
// by how much is policy, owned by the callers:
//   garage temp / humidity / VOC index: 30 samples at 1 Hz  (~30 s)
//   battery volts / percent:             3 reads at 30 s     (~90 s)
//   outdoor temp, barometer, plug watts: NOT smoothed -- trusted as-is.
//
// The caller pushes only valid readings (no NaN, no sentinels); a failed
// read simply pushes nothing, so the mean coasts on the last good samples
// instead of absorbing garbage. Pure header (no Arduino includes) so the
// native test env compiles this exact logic -- see auto_logic.h precedent.

#include <cmath>

namespace sensors {

template <int N>
struct RollingAvg {
  static_assert(N > 0, "window must hold at least one sample");
  float buf[N];
  int count = 0;  // valid samples held, saturates at N
  int head = 0;   // next slot to overwrite

  void push(float v) {
    buf[head] = v;
    head = (head + 1) % N;
    if (count < N)
      count++;
  }

  // Forget everything: on sensor death the stale mean must die with it.
  void reset() {
    count = 0;
    head = 0;
  }

  bool empty() const { return count == 0; }

  // Mean of what is held; NAN when nothing is. Until the window fills the
  // mean covers only the samples seen so far -- a fresh boot answers from
  // its first reading, it does not wait 30 s to have an opinion.
  float avg() const {
    if (count == 0)
      return NAN;
    float sum = 0;
    for (int i = 0; i < count; i++)
      sum += buf[i];
    return sum / count;
  }
};

}  // namespace sensors
