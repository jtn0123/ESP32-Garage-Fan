#pragma once
// The STEMMA air-quality chain, 18 inches off the board so the readings are
// of the GARAGE, not of the board's own heat: an SHT41 (temperature/humidity,
// 0x44) and an SGP41 (MOX VOC/NOx, 0x59), added 2026-08-13.
//
// The SHT41 exists to retire the BME280 self-heating offsets: it hangs far
// enough from the board that its reading needs no correction, so when it is
// present climate:: uses it raw and the T-OFFSET knobs only matter for the
// BME280 fallback. The BME280 stays for pressure either way.
//
// The SGP41 is sampled at 1 Hz -- Sensirion's gas-index algorithm is
// specified at that cadence -- with the first 10 s spent in its conditioning
// command. Raw ticks are meaningful immediately; the VOC index needs about an
// hour of warm-up before it moves and the NOx index several, so both raws are
// logged and charted alongside the indices rather than hiding the warm-up.
#include <stdint.h>

namespace air {

/** Probe the bus and start whichever sensors answered. Call once, after Wire
 *  is up. Absent hardware is a supported configuration, not an error. */
void begin();

/** 1 Hz worker: SGP41 conditioning, then raw measurement into the two gas
 *  index algorithms, with SHT41 compensation when available. Rate-limits
 *  itself; call from the loop. */
void tick();

bool sht_ok();
bool sgp_ok();

/** SHT41 temperature (C) / humidity (%RH); NAN when absent or failed. */
float temp_c();
float rh();

/** Raw SGP41 ticks, meaningful from the first sample. -1 when absent. */
int32_t voc_raw();
int32_t nox_raw();

/** Sensirion gas indices, 1..500 (100 = typical). 0 while the algorithm is
 *  still warming up, -1 when the sensor is absent. */
int32_t voc_index();
int32_t nox_index();

}  // namespace air
