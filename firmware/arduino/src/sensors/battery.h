#pragma once
// The backup pack's fuel gauge, and the sticky charging verdict derived from
// it. Owns all of its own state; nothing outside this module touches the I2C
// registers or the history buffers.
//
// The charging verdict matters far beyond the battery readout: the BME280 sits
// on the same board and runs several degrees warmer while charging, so
// climate::corrected() picks its offset from battery::charging(). A detector
// that flaps swings garage temperature by ~12 C and poisons every consumer --
// tiles, ring buffer, auto mode. Hence the hysteresis and the NVS persistence.

#include <Preferences.h>
#include <stdint.h>

namespace battery {

/** Which gauge answered. 0 none, 1 MAX17048, 2 LC709203F, 3 raw LC registers. */
uint8_t kind();

/** Probe for a gauge. Cheap and idempotent once one has been found. */
void begin();

/** Sample voltage and percent. No-op when no gauge is present. */
void read();

/** Take a 5-minute history point and re-evaluate the charging verdict. */
void sample();

float volts();    // NAN when unknown
float percent();  // NAN when the gauge does not report SoC
bool charging();

/** Millivolts per hour over the sliding window, or NAN before it fills. */
float slope_mv_per_h();

/**
 * Hours of runtime left at the current discharge slope.
 * Sets *out_charging always; leaves *out_eta_h as NAN while charging or flat.
 */
void eta(bool* out_charging, float* out_eta_h);

/** Restore the persisted charging verdict. Call once, before the first sample. */
void restore(Preferences* prefs);

// --- raw LC709203F access, exposed only for the /api/battdebug probe --------
// The Adafruit library's begin() rejects this board's chip on an IC-version
// check even though it ACKs, so the raw path is the one that actually works.
uint8_t crc8(const uint8_t* data, size_t n);
bool write_reg(uint8_t reg, uint16_t value);
bool read_reg(uint8_t reg, uint16_t* out);

}  // namespace battery
