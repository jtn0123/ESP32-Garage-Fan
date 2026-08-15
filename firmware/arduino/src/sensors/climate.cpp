// See climate.h. The BME280 is barometer-only: begin-on-demand, humidity
// conversion disabled at the chip, and its thermometer read only to satisfy
// the pressure compensation maths (t_fine), never surfaced.
#include "sensors/climate.h"

#include <Adafruit_BME280.h>
#include <Arduino.h>
#include <Wire.h>

#include <cmath>

#include "config.h"
#include "sensors/air.h"
#include "system/timeutil.h"

namespace climate {
namespace {

Adafruit_BME280 g_bme;
bool g_ok = false;
float g_inside_c = NAN;
// When g_inside_c was last actually measured. 0 = never. Without an expiry a
// wedged sensor froze the reading FOREVER and the fan ran on hours-old data
// all night (2026-08-11); NaN is the contract fan_auto_decide expects.
uint32_t g_inside_ms = 0;
constexpr uint32_t kInsideStaleMs = 10 * 60 * 1000;  // two missed samples
float g_outside_c = NAN;
uint32_t g_outside_ms = 0;
time_t g_outdoor_epoch = 0;  // bridge's own timestamp topic, if any
uint32_t g_epoch_rx_ms = 0;  // when that /ts arrived, for pairing

// How close together a /ts and its temperature must land to count as one
// sample. Broker delivery of a pair is millisecond-scale; ten seconds is
// generous without letting an epoch outlive its own sample cycle.
constexpr uint32_t kEpochPairMs = 10000;

bool begin_if_needed() {
  if (g_ok)
    return true;
  g_ok = g_bme.begin(0x77, &Wire) || g_bme.begin(0x76, &Wire);
  if (g_ok) {
    // Barometer-only sampling: forced mode (the chip sleeps between reads),
    // temperature at x1 ONLY because the pressure compensation needs t_fine,
    // and humidity conversion OFF entirely -- "don't waste power on it".
    g_bme.setSampling(Adafruit_BME280::MODE_FORCED, Adafruit_BME280::SAMPLING_X1,
                      Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::SAMPLING_NONE);
    Serial.println("bme280 barometer detected");
  }
  return g_ok;
}

}  // namespace

void restore(Preferences* prefs) {
  (void)prefs;  // nothing persisted here since the offsets died with the BME thermometer
}

bool sample(float* t, float* h, float* p) {
  // The SHT41 IS the garage temperature and humidity; no fallback. A sample
  // without it is a failed sample, and auto holds rather than guesses.
  if (!air::sht_ok() || isnan(air::temp_c()) || isnan(air::rh()))
    return false;
  *t = air::temp_c();
  *h = air::rh();
  g_inside_c = *t;
  g_inside_ms = millis();
  // Pressure is best-effort: absent or implausible reads NAN (the CSV's -999,
  // the chart's gap) and never blocks the climate sample. Out-of-range means
  // a failed I2C transaction compensated into a finite number, not weather
  // (-162.9 hPa reached the broker on 2026-08-09).
  *p = NAN;
  if (begin_if_needed()) {
    g_bme.takeForcedMeasurement();
    const float pv = g_bme.readPressure() / 100.0f;
    if (isnan(pv) || pv < 300.0f || pv > 1100.0f)
      g_ok = false;  // wedged or unplugged; re-probe next sample
    else
      *p = pv;
  }
  return true;
}

void refresh_inside() {
  if (air::sht_ok() && !isnan(air::temp_c())) {
    g_inside_c = air::temp_c();
    g_inside_ms = millis();
  }
}

float inside_c() {
  // Symmetric with outside_c_fresh(): a reading nobody has been able to
  // refresh is not a reading.
  if (g_inside_ms == 0 || millis() - g_inside_ms > kInsideStaleMs)
    return NAN;
  return g_inside_c;
}
bool ok() { return g_ok; }

void set_outside_f(float f) {
  g_outside_c = (f - 32.0f) * 5.0f / 9.0f;
  g_outside_ms = millis();
  // Pair the bridge timestamp with its sample: keep an epoch that arrived
  // just before this temperature (broker delivers the retained or live pair
  // within milliseconds, in either order), discard one that is older than a
  // pairing window. So: retained replay keeps its stale epoch and reads
  // stale; a live feed keeps its fresh epoch; and a bridge that STOPS
  // publishing /ts falls back to receipt-time freshness within one sample
  // instead of gating on a fossil epoch forever.
  if (millis() - g_epoch_rx_ms > kEpochPairMs)
    g_outdoor_epoch = 0;
}

void set_outdoor_epoch(long epoch) {
  g_outdoor_epoch = epoch;
  g_epoch_rx_ms = millis();
}

float outside_c_fresh() {
  // A bridge that publishes its own epoch timestamp gives real freshness --
  // retained redelivery of an old snapshot reads as stale, as it should.
  // Feeds without a ts topic fall back to receipt-time freshness.
  if (g_outdoor_epoch > 0) {
    if (!time_synced() || time(nullptr) - g_outdoor_epoch > 1800)
      return NAN;
    return g_outside_c;
  }
  if (g_outside_ms == 0 || millis() - g_outside_ms > kOutdoorStaleMs)
    return NAN;
  return g_outside_c;
}

}  // namespace climate
