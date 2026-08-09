#pragma once
// The sliding window and sticky charging verdict behind sensors/battery, kept
// Arduino-free so the native_battery_logic env can replay charge/discharge
// tapes on the host. sensors/battery.cpp owns the I2C reads, the NVS persist
// and the one device instance; every number that decides anything lives here.
//
// Why the verdict is sticky: the BME280 shares the board and runs ~12 C
// warmer while charging, so climate::corrected() keys its offset off this
// verdict. A flapping detector poisons every consumer of garage temperature
// (tiles, ring, auto mode). Enter on clearly rising evidence, leave on
// clearly falling, and in the plateau between (trickle charge, jitter) the
// last verdict stands.

#include <stdint.h>

#include <cmath>
#include <cstring>

namespace battery {

inline constexpr uint8_t kWinLen = 12;  // 5-min cadence -> 1 h sliding window

struct Window {
  float pct[kWinLen];
  float v[kWinLen];
  uint8_t n = 0;

  /**
   * Push one 5-min point. Never writes a real number for an unknown percent:
   * carries the last known value through a transient failed read, and seeds
   * with NAN before RSOC has ever answered. A fabricated 0 in the window
   * reads as a huge charge or discharge and flips the sticky verdict.
   */
  void push(float volts, float percent) {
    if (n == kWinLen) {
      memmove(pct, pct + 1, (kWinLen - 1) * sizeof(float));
      memmove(v, v + 1, (kWinLen - 1) * sizeof(float));
      n--;
    }
    pct[n] = std::isnan(percent) ? (n ? pct[n - 1] : NAN) : percent;
    v[n] = volts;
    n++;
  }

  /** Millivolts per hour across the window, or NAN before 3 points. */
  float slope_mv_per_h(float volts_now) const {
    if (n < 3)
      return NAN;
    const float hours = (n - 1) * 5.0f / 60.0f;
    return (volts_now - v[0]) * 1000.0f / hours;
  }

  /**
   * Percent drained across the window, + = draining. NAN endpoints (RSOC
   * never answered) contribute no percent signal and read as 0: the voltage
   * slope alone drives the verdict then.
   */
  float drain_pct() const {
    if (!n)
      return 0.0f;
    const float p_old = pct[0], p_new = pct[n - 1];
    return (std::isnan(p_old) || std::isnan(p_new)) ? 0.0f : p_old - p_new;
  }
};

/**
 * The sticky charging verdict, re-judged once per 5-min sample. +5 mV/h is
 * unmistakably inbound power on a big pack; 4.17 V is float voltage. Holds
 * `prev` before 3 points and in the plateau.
 */
inline bool judge_charging(const Window& w, float volts_now, bool prev) {
  if (w.n < 3)
    return prev;
  const float mvh = w.slope_mv_per_h(volts_now);
  const float dpct = w.drain_pct();
  if (mvh >= 5.0f || volts_now >= 4.17f || dpct < -0.3f)
    return true;
  if (mvh <= -5.0f || dpct > 0.3f)
    return false;
  return prev;
}

/**
 * Hours of runtime left at the window's drain rate. NAN while charging,
 * before 6 points, without RSOC at both endpoints, or when the drain across
 * the window is inside measurement jitter (<= 0.2 pct).
 */
inline float eta_hours(const Window& w, float percent_now, bool charging) {
  if (w.n < 6 || charging)
    return NAN;
  const float p_old = w.pct[0], p_new = w.pct[w.n - 1];
  if (std::isnan(p_old) || std::isnan(p_new))
    return NAN;
  const float delta = p_old - p_new;  // + = draining
  if (delta <= 0.2f)
    return NAN;
  const float hours = (w.n - 1) * 5.0f / 60.0f;
  return percent_now / (delta / hours);
}

}  // namespace battery
