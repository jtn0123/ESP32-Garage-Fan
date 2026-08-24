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

/**
 * One outdoor poll, degF on the wire. The firmware's own open-meteo fetch
 * (net/weather) is the ONLY caller: the Home Assistant relay that used to
 * race it was removed in 1.23.0 -- see sensors/outdoor.h for what that race
 * did to the differential.
 */
void set_outside_f(float f);

/** Outdoor temperature in degC, smoothed over the last polls; NAN once the
 *  feed goes stale, which auto reads as "hold, never guess". */
float outside_c_fresh();

/** The last raw poll in degC, unsmoothed -- telemetry only, never control. */
float outside_c_raw();

/** True when no usable outdoor reading is held (the fan is flying blind). */
bool outside_stale();

}  // namespace climate
