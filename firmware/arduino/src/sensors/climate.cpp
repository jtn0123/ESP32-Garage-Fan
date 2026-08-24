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
#include "sensors/outdoor.h"

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
// Three polls at weather.cpp's 10-minute cadence: ~30 minutes of coverage,
// which is what it takes to blunt open-meteo's own >= 1 degF poll steps
// against a 1 degF hysteresis band. outdoor.h owns the reasoning.
sensors::OutdoorFeed<3, kOutdoorStaleMs> g_outside;

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

void set_outside_f(float f) { g_outside.push((f - 32.0f) * 5.0f / 9.0f, millis()); }

// Receipt time is exact freshness now that the firmware owns the only writer.
// The bridge-epoch pairing this replaced existed because a RETAINED MQTT
// message replays at connect looking brand new while carrying yesterday's
// weather; with the relay gone there is no retained replay to defend against.
float outside_c_fresh() { return g_outside.value_c(millis()); }

float outside_c_raw() { return g_outside.raw_c(millis()); }

bool outside_stale() { return g_outside.stale(millis()); }

}  // namespace climate
