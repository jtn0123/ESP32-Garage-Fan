// See air.h.
#include "sensors/air.h"

#include <Adafruit_SHT4x.h>
#include <Arduino.h>
#include <NOxGasIndexAlgorithm.h>
#include <SensirionI2CSgp41.h>
#include <VOCGasIndexAlgorithm.h>
#include <Wire.h>

#include <cmath>

#include "system/eventlog.h"

namespace air {
namespace {

Adafruit_SHT4x g_sht;
SensirionI2CSgp41 g_sgp;
VOCGasIndexAlgorithm g_voc_algo;
NOxGasIndexAlgorithm g_nox_algo;

bool g_sht_ok = false;
bool g_sgp_ok = false;
uint32_t g_last_tick_ms = 0;
uint8_t g_conditioning_left = 10;  // SGP41 datasheet: 10 s of conditioning first

float g_t = NAN;
float g_h = NAN;
int32_t g_voc_raw = -1;
int32_t g_nox_raw = -1;
int32_t g_voc = -1;
int32_t g_nox = -1;
uint8_t g_sht_fail = 0;
uint8_t g_sgp_fail = 0;

}  // namespace

void begin() {
  g_sht_ok = g_sht.begin(&Wire);
  if (g_sht_ok) {
    // High precision is ~8 ms per read at 1 Hz -- nothing on this loop cares.
    g_sht.setPrecision(SHT4X_HIGH_PRECISION);
    g_sht.setHeater(SHT4X_NO_HEATER);  // the whole point is an unheated reading
  }
  g_sgp.begin(Wire);
  uint16_t test = 0;
  g_sgp_ok = g_sgp.executeSelfTest(test) == 0 && test == 0xD400;
  eventlog::log("air", "sht41=%s sgp41=%s", g_sht_ok ? "ok" : "absent", g_sgp_ok ? "ok" : "absent");
}

void tick() {
  if (millis() - g_last_tick_ms < 1000)
    return;
  g_last_tick_ms = millis();

  if (g_sht_ok) {
    sensors_event_t hum;
    sensors_event_t temp;
    if (g_sht.getEvent(&hum, &temp)) {
      g_t = temp.temperature;
      g_h = hum.relative_humidity;
      g_sht_fail = 0;
    } else if (++g_sht_fail >= 10) {
      // Ten straight misses is a detached cable, not a glitch. Say so once
      // and fall back to the BME280 path rather than serving stale numbers.
      eventlog::log("air", "sht41 stopped answering; falling back to bme280");
      g_sht_ok = false;
      g_t = NAN;
      g_h = NAN;
    }
  }

  if (!g_sgp_ok)
    return;
  // Compensation in the SGP41's fixed-point format; datasheet defaults when
  // the SHT41 has no reading yet.
  const float t = isnan(g_t) ? 25.0f : g_t;
  const float h = isnan(g_h) ? 50.0f : g_h;
  const uint16_t rh_ticks = static_cast<uint16_t>(h * 65535.0f / 100.0f);
  const uint16_t t_ticks = static_cast<uint16_t>((t + 45.0f) * 65535.0f / 175.0f);
  uint16_t voc = 0;
  uint16_t nox = 0;
  uint16_t err;
  if (g_conditioning_left > 0) {
    // NOx needs its hotplate conditioned before the first real measurement.
    err = g_sgp.executeConditioning(rh_ticks, t_ticks, voc);
    if (err == 0)
      --g_conditioning_left;
    return;
  }
  err = g_sgp.measureRawSignals(rh_ticks, t_ticks, voc, nox);
  if (err != 0) {
    if (++g_sgp_fail >= 10) {
      eventlog::log("air", "sgp41 stopped answering; gas readings disabled");
      g_sgp_ok = false;
    }
    return;
  }
  g_sgp_fail = 0;
  g_voc_raw = voc;
  g_nox_raw = nox;
  g_voc = g_voc_algo.process(voc);
  g_nox = g_nox_algo.process(nox);
}

bool sht_ok() { return g_sht_ok; }
bool sgp_ok() { return g_sgp_ok; }
float temp_c() { return g_sht_ok ? g_t : NAN; }
float rh() { return g_sht_ok ? g_h : NAN; }
int32_t voc_raw() { return g_sgp_ok ? g_voc_raw : -1; }
int32_t nox_raw() { return g_sgp_ok ? g_nox_raw : -1; }
int32_t voc_index() { return g_sgp_ok ? g_voc : -1; }
int32_t nox_index() { return g_sgp_ok ? g_nox : -1; }

}  // namespace air
