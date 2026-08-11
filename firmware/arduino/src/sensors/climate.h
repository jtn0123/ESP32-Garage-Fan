#pragma once
// Garage climate (the on-board BME280) and the yard temperature that arrives
// over MQTT. Owns both readings and the self-heating correction between them.
//
// The correction is why inside and outside live together: every consumer of
// "garage temperature" must see the same corrected number, and the correction
// itself depends on battery::charging() -- the board runs several degrees
// warmer while the pack charges. History is stored corrected, so changing an
// offset never rewrites what was already logged.

#include <Preferences.h>

namespace climate {

/** Load the two offsets from NVS and remember where to persist them. */
void restore(Preferences* prefs);

/**
 * One 5-minute sample: begin-on-demand, read, correct.
 * False when the sensor is absent or answered NaN (it will re-probe next
 * call). On success *t is corrected °C, *h %RH, *p hPa.
 */
bool sample(float* t, float* h, float* p);

/** Cheap temperature-only refresh for the 30 s auto tick. */
void refresh_inside();

/**
 * Last corrected garage temperature, °C.
 *
 * NAN before the first good read AND once that read goes stale (10 min, two
 * missed sample cycles) -- a wedged or unplugged BME280 must not keep handing
 * the thermostat a confident number it will act on all night.
 */
float inside_c();

/** Did the BME280 answer its last transaction? */
bool ok();

/** Apply the currently active self-heating offset to a raw reading. */
float corrected(float raw_c);

float offset_charging();
float offset_idle();
/** The offset in effect right now, picked by battery::charging(). */
float offset_active();
void set_offset_charging(float c);
void set_offset_idle(float c);

/** Yard reading from MQTT, already °F on the wire. */
void set_outside_f(float f);
/** Bridge-published epoch for the yard reading, when the feed provides one. */
void set_outdoor_epoch(long epoch);
/**
 * Yard temperature in °C, or NAN once the reading is stale (30 min by
 * receipt time, or by the bridge's own timestamp when it publishes one --
 * retained redelivery of an old snapshot must read as stale).
 */
float outside_c_fresh();

}  // namespace climate
