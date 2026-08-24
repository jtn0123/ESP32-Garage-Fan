#pragma once
// Copyright 2026 Justin
//
// The outdoor temperature the differential thermostat subtracts from the
// garage reading -- one source, smoothed, with an explicit expiry.
//
// Until 1.23.0 two writers raced for this value: the firmware's own
// open-meteo poll and a `home/outdoor/temp_f` feed relayed by Home Assistant,
// last write wins (`weather.h`: "freshest write wins"). Both were weather
// SERVICES -- the MQTT topic tree carried `weather`, `condition_code` and
// `wind_mps` beside the temperature -- and the two models disagreed about the
// afternoon by a median 0.9 degF and up to 4.5 degF, converging only around
// dawn. The differential the latch compares therefore alternated across the
// whole 1 degF hysteresis band every five minutes, purely on which feed wrote
// last (measured 2026-08-21 16:41-18:16: nine commanded latch reversals in
// fifty-five minutes while the garage reading moved 0.1 degF). The MQTT feed
// is gone; this holds what is left.
//
// TWO mechanisms, because one source is not enough on its own:
//
//   - The MEAN. open-meteo's own poll-to-poll step is >= 1.0 degF on 22% of
//     polls (p90 1.4 degF) -- wider than the band by itself, so dropping to a
//     single feed removed only ~9% of the churn in replay. N polls of
//     coverage turns one 1.4 degF step into N steps of 1.4/N.
//   - The EXPIRY. A value nobody has refreshed is not a reading:
//     `fan_auto_decide` takes NAN as "hold speed and latch, never guess", and
//     that is the correct answer to a dead feed. Receipt time is exact now
//     that the firmware owns the only writer -- the old bridge-epoch pairing
//     existed because a retained MQTT replay could look fresh while carrying
//     yesterday's weather, and there is no retained replay any more.
//
// A gap longer than the expiry also CLEARS the window: samples from before an
// outage describe different weather, and blending them into the first reading
// back is worse than having no opinion for one poll.
//
// Pure header (no Arduino includes) so the native test env compiles this
// exact logic -- see auto_logic.h and rolling.h for the precedent.

#include <cmath>
#include <cstdint>

#include "sensors/rolling.h"

namespace sensors {

/**
 * @tparam N        polls held in the mean. weather.cpp polls every 10 min, so
 *                  3 covers ~30 min -- long enough to span the disturbance,
 *                  short enough that a real front still moves the fan.
 * @tparam StaleMs  how long a reading stays usable after it arrives.
 */
template <int N, uint32_t StaleMs>
struct OutdoorFeed {
  static_assert(N > 0, "window must hold at least one poll");
  static_assert(StaleMs > 0, "a reading has to expire eventually");

  RollingAvg<N> ring;
  float last_c = NAN;
  uint32_t last_ms = 0;
  bool ever = false;  // distinct from last_ms == 0, a legal millis() value

  /** Record one poll. `now_ms` is millis() at receipt. */
  void push(float c, uint32_t now_ms) {
    if (std::isnan(c))
      return;  // a failed fetch pushes nothing; the mean coasts
    if (ever && now_ms - last_ms > StaleMs)
      ring.reset();  // the gap outlived the samples
    ring.push(c);
    last_c = c;
    last_ms = now_ms;
    ever = true;
  }

  /** True when nothing usable is held -- auto must hold, not guess. */
  bool stale(uint32_t now_ms) const { return !ever || now_ms - last_ms > StaleMs; }

  /** The smoothed reading, NAN once stale. What the thermostat compares. */
  float value_c(uint32_t now_ms) const {
    if (stale(now_ms))
      return NAN;
    return ring.avg();
  }

  /** The last raw poll, unsmoothed -- for telemetry, never for control. */
  float raw_c(uint32_t now_ms) const { return stale(now_ms) ? NAN : last_c; }

  /** Forget everything (feed disabled, or coordinates cleared). */
  void reset() {
    ring.reset();
    last_c = NAN;
    last_ms = 0;
    ever = false;
  }
};

/**
 * Edge detector over a feed's staleness, so the flight recorder gets one line
 * each way instead of one per poll.
 *
 * It lives here rather than as a static bool in weather.cpp because the going-
 * blind moment is the whole safety story of a single-source feed: auto does
 * the right thing silently (NAN holds speed and latch), and a fan that quietly
 * stopped answering the weather is precisely the five-day failure the direct
 * poll was written to end. A behaviour that important gets a test.
 */
struct BlindEdge {
  enum Edge { kNone, kWentBlind, kRestored };

  bool was_blind = false;
  bool ever = false;  // the first observation is an edge only if it is blind

  Edge update(bool blind) {
    const bool first = !ever;
    ever = true;
    if (!first && blind == was_blind)
      return kNone;
    was_blind = blind;
    if (first)
      return blind ? kWentBlind : kNone;  // booting healthy is not news
    return blind ? kWentBlind : kRestored;
  }
};

}  // namespace sensors
