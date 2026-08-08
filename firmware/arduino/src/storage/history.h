#pragma once
// The 24-hour in-RAM ring: one row per 5-minute sample, the source behind
// /api/history days=1, /api/sensors, /download.csv, the stats tiles and the
// e-ink panel. Survives broker outages by construction (it is RAM, written
// before anything is published); does not survive reboots -- the console's
// localStorage merge papers over that on the browser side.
//
// Accessors hand out the raw arrays on purpose: the JSON writers iterate
// hundreds of rows and a per-element function call buys nothing but stack
// churn on a 240 MHz core serving HTTP in the loop thread.

#include <stdint.h>

#include <ctime>

namespace history {

struct Sample {
  float t;       // corrected garage temperature, C
  float h;       // %RH
  float p;       // hPa
  float out_f;   // yard, F (NAN = no fresh feed at sample time)
  float batt_v;  // NAN = no fuel gauge
  int8_t speed;  // fan speed 0..12 at sample time
  int8_t chg;    // 1 charging, 0 not, -1 no battery
};

/** Append a row, evicting the oldest once full. `stamped` = clock is synced. */
void append(const Sample& s, bool stamped);

uint16_t count();
time_t end_ts();  // epoch of the newest row, 0 before SNTP sync

const float* temp();
const float* rh();
const float* hpa();
const float* out_f();
const float* batt_v();
const int8_t* speed();
const int8_t* chg();

/** Min/max/avg of the temperature column; NANs when the ring is empty. */
void temp_stats(float* mn, float* mx, float* avg);

}  // namespace history
