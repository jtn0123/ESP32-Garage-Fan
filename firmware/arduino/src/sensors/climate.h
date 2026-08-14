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

/**
 * The BME280's own last estimate (offset-corrected), NAN before the first
 * sample. Logged beside the SHT41 so the two thermometers can be compared on
 * the charts; when the SHT41 is absent this IS the primary reading.
 */
float bme_temp_c();
float bme_rh();

/**
 * Which thermometer drives auto mode and the displays. Defaults to the
 * off-board SHT41 whenever it answers (its reading needs no self-heating
 * offset); switching it off forces the BME280 + offset path -- the escape
 * hatch if the new sensor misbehaves. Effective source is /api/sensors'
 * `source`; this is the PREFERENCE.
 */
bool prefer_sht();
void set_prefer_sht(bool on);

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
