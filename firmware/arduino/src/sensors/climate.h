#pragma once
// Garage climate and the yard temperature that arrives over MQTT.
//
// The temperature/humidity source is the OFF-BOARD SHT41 (sensors/air), full
// stop. The on-board BME280 serves only as the barometer: its thermometer
// read +5degC high whenever the fan rested (board self-heating, proven on the
// 2026-08-14 charts) and the offsets that existed to correct it never could
// -- the error tracked airflow, not charge state. Humidity conversion is
// disabled in the chip's sampling config; it does not even run.

#include <Preferences.h>

namespace climate {

void restore(Preferences* prefs);

/**
 * One 5-minute sample. *t / *h from the SHT41 -- false when it has no
 * answer (auto then holds, never guesses; there is no fallback thermometer
 * anymore). *p from the BME280 barometer, NAN when that chip is absent or
 * wedged: pressure is best-effort and never blocks the sample.
 */
bool sample(float* t, float* h, float* p);

/** Cheap refresh for the 30 s auto tick (SHT41). */
void refresh_inside();

/** Garage temperature, degC; NAN once the reading is stale (never guess). */
float inside_c();

/** Did the BME280 barometer answer its last transaction? */
bool ok();

/** Yard reading from MQTT, already degF on the wire. */
void set_outside_f(float f);
/** Bridge-published epoch for the yard reading, when the feed provides one. */
void set_outdoor_epoch(long epoch);
/** Yard temperature in degC, NAN once stale (30 min, or by bridge epoch). */
float outside_c_fresh();

}  // namespace climate
