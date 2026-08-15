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
//
// The ring itself (append/evict/stats) lives in ring_logic.h so the
// native_storage_ring env can test it on the host; this module owns the one
// device-sized instance and the SNTP end-timestamp.

#include <stdint.h>

#include <ctime>

#include "storage/ring_logic.h"

namespace history {

/** Append a row, evicting the oldest once full. `stamped` = clock is synced. */
void append(const Sample& s, bool stamped);

uint16_t count();
time_t end_ts();  // epoch of the newest row, 0 before SNTP sync

const float* temp();
const float* rh();
const float* hpa();
const float* out_f();
const float* batt_v();
const float* watts();
const float* bme_t();
const float* bme_rh();
const int32_t* voc_raw();
const int32_t* nox_raw();
const int16_t* voc();
const int16_t* nox();
const int8_t* speed();
const int8_t* chg();

/** Min/max/avg of the temperature column; NANs when the ring is empty. */
void temp_stats(float* mn, float* mx, float* avg);

}  // namespace history
