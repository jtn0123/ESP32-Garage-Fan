// See climate.h. The BME280 begin-on-demand pattern and the freshness rules
// are carried over verbatim from the pre-split firmware.
#include "sensors/climate.h"

#include <Adafruit_BME280.h>
#include <Arduino.h>
#include <Wire.h>

#include <cmath>

#include "config.h"
#include "sensors/battery.h"
#include "system/timeutil.h"

namespace climate {
namespace {

Adafruit_BME280 g_bme;
Preferences* g_prefs = nullptr;
bool g_ok = false;
float g_inside_c = NAN;
// Board self-heating correction: charging current warms the board-mounted
// BME280 far more than idle operation does. User-tunable via
// /api/config?offc=&offi= (Celsius, added to the raw reading).
float g_off_chg = -3.0f;
float g_off_idle = -1.0f;
float g_outside_c = NAN;
uint32_t g_outside_ms = 0;
time_t g_outdoor_epoch = 0;  // bridge's own timestamp topic, if any

bool begin_if_needed() {
  if (g_ok)
    return true;
  g_ok = g_bme.begin(0x77, &Wire) || g_bme.begin(0x76, &Wire);
  if (g_ok)
    Serial.println("bme280 detected");
  return g_ok;
}

}  // namespace

void restore(Preferences* prefs) {
  g_prefs = prefs;
  if (prefs) {
    g_off_chg = prefs->getFloat("offc", -3.0f);
    g_off_idle = prefs->getFloat("offi", -1.0f);
  }
}

float corrected(float raw_c) { return raw_c + offset_active(); }

bool sample(float* t, float* h, float* p) {
  if (!begin_if_needed())
    return false;
  const float t_raw = g_bme.readTemperature();
  const float hv = g_bme.readHumidity();
  const float pv = g_bme.readPressure() / 100.0f;
  if (isnan(t_raw) || isnan(hv) || isnan(pv)) {
    g_ok = false;  // sensor wedged or unplugged; re-probe next sample
    return false;
  }
  *t = corrected(t_raw);
  *h = hv;
  *p = pv;
  g_inside_c = *t;
  return true;
}

void refresh_inside() {
  if (!g_ok)
    return;
  const float t = g_bme.readTemperature();
  if (!isnan(t))
    g_inside_c = corrected(t);
}

float inside_c() { return g_inside_c; }
bool ok() { return g_ok; }

float offset_charging() { return g_off_chg; }
float offset_idle() { return g_off_idle; }
float offset_active() { return battery::charging() ? g_off_chg : g_off_idle; }

void set_offset_charging(float c) {
  g_off_chg = c;
  if (g_prefs)
    g_prefs->putFloat("offc", c);
}

void set_offset_idle(float c) {
  g_off_idle = c;
  if (g_prefs)
    g_prefs->putFloat("offi", c);
}

void set_outside_f(float f) {
  g_outside_c = (f - 32.0f) * 5.0f / 9.0f;
  g_outside_ms = millis();
  // Each temperature clears the bridge timestamp; a feed that publishes /ts
  // re-sends it alongside every sample, so epoch mode re-arms immediately.
  // A bridge that STOPS publishing /ts (config change) falls back to
  // receipt-time freshness instead of gating on a fossil epoch forever, and
  // retained replay still reads stale: the redelivered old /ts re-arms epoch
  // mode with its old value.
  g_outdoor_epoch = 0;
}

void set_outdoor_epoch(long epoch) { g_outdoor_epoch = epoch; }

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
