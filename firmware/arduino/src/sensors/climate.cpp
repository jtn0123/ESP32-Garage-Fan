// See climate.h. The BME280 begin-on-demand pattern and the freshness rules
// are carried over verbatim from the pre-split firmware.
#include "sensors/climate.h"

#include <Adafruit_BME280.h>
#include <Arduino.h>
#include <Wire.h>

#include <cmath>

#include "config.h"
#include "sensors/air.h"
#include "sensors/battery.h"
#include "system/timeutil.h"

namespace climate {
namespace {

float g_bme_t = NAN;   // the BME280's own corrected estimate, kept for comparison
float g_bme_rh = NAN;
// Which thermometer drives auto and the displays. True (default) prefers the
// off-board SHT41 whenever it answers; false forces the BME280 + offsets --
// the escape hatch if the new sensor ever reads wrong.
bool g_prefer_sht = true;

bool use_sht() { return g_prefer_sht && air::sht_ok() && !isnan(air::temp_c()); }

Adafruit_BME280 g_bme;
Preferences* g_prefs = nullptr;
bool g_ok = false;
float g_inside_c = NAN;
// When g_inside_c was last actually measured. 0 = never.
//
// Without this the garage reading had no expiry at all, while the yard reading
// had two (outside_c_fresh). So a BME280 that wedged or came unplugged froze
// g_inside_c at its last value FOREVER, and fan_auto_decide -- which is
// written to hold speed and latch on NaN precisely so it never guesses -- kept
// being handed a confident number. Sensor drops out at 4 PM with the garage at
// 32 C, the yard cools to 15 C overnight, and the delta stays above the engage
// threshold every 30 s tick: the fan runs at auto_max all night on a reading
// hours old. /api/state said "sensor":false the whole time; nothing in the
// control path read it.
uint32_t g_inside_ms = 0;

// Two missed 5-minute samples. Long enough that the transient I2C
// ESP_ERR_INVALID_STATE pairs this board logs every ~30 s cannot trip it (a
// failed read leaves g_ok false and the next sample() re-probes), short enough
// that a genuinely dead sensor stops driving the fan within minutes.
constexpr uint32_t kInsideStaleMs = 10 * 60 * 1000;
// Board self-heating correction: charging current warms the board-mounted
// BME280 far more than idle operation does. User-tunable via
// /api/config?offc=&offi= (Celsius, added to the raw reading).
float g_off_chg = -3.0f;
float g_off_idle = -1.0f;
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
    g_prefer_sht = prefs->getBool("sht", true);
  }
}

float corrected(float raw_c) { return raw_c + offset_active(); }

// NaN is not the only way a BME280 read fails, and assuming it was let bad
// data all the way through. When the I2C transaction is refused -- this rig
// logs ESP_ERR_INVALID_STATE regularly -- the driver still runs the
// compensation maths, on garbage, and returns a perfectly finite number.
// 167.25 C, 100 %RH and -162.9 hPa reached the RAM ring, the SD CSV, the
// broker and the auto thermostat on 2026-08-09, and /api/sensors reported
// them as a measurement. Outside the datasheet's operating range it is a
// failed read, not weather.
static bool plausible(float t_c, float rh, float hpa) {
  return !isnan(t_c) && !isnan(rh) && !isnan(hpa) && t_c > -40.0f && t_c < 85.0f && rh >= 0.0f &&
         rh <= 100.0f && hpa > 300.0f && hpa < 1100.0f;
}

bool sample(float* t, float* h, float* p) {
  if (!begin_if_needed())
    return false;
  const float t_raw = g_bme.readTemperature();
  const float hv = g_bme.readHumidity();
  const float pv = g_bme.readPressure() / 100.0f;
  if (!plausible(t_raw, hv, pv)) {
    g_ok = false;  // sensor wedged or unplugged; re-probe next sample
    return false;
  }
  // Keep the BME280's own estimate (offset-corrected) regardless of which
  // sensor wins below: both get logged side by side so the two thermometers
  // can be compared on the charts instead of argued about.
  g_bme_t = corrected(t_raw);
  g_bme_rh = hv;
  // The SHT41 hangs 18 inches off the board precisely so its reading needs no
  // self-heating correction; when it answers, it IS the garage temperature
  // and humidity, and the offset knobs only matter for this BME280 fallback.
  // Pressure always comes from the BME280 -- the SHT41 has no barometer.
  if (use_sht()) {
    *t = air::temp_c();
    *h = air::rh();
  } else {
    *t = g_bme_t;
    *h = hv;
  }
  *p = pv;
  g_inside_c = *t;
  g_inside_ms = millis();
  return true;
}

float bme_temp_c() { return g_bme_t; }
float bme_rh() { return g_bme_rh; }

bool prefer_sht() { return g_prefer_sht; }

void set_prefer_sht(bool on) {
  g_prefer_sht = on;
  if (g_prefs)
    g_prefs->putBool("sht", on);
}

void refresh_inside() {
  // SHT41 first, same preference as sample() -- and the same reason.
  if (use_sht()) {
    g_inside_c = air::temp_c();
    g_inside_ms = millis();
    return;
  }
  if (!g_ok)
    return;
  const float t = g_bme.readTemperature();
  // Same guard as sample(): this value feeds the auto thermostat every 30 s,
  // so a garbage read here would drive the fan directly. Holding the last
  // good reading is what auto_logic already expects of missing data.
  if (t > -40.0f && t < 85.0f && !isnan(t)) {
    g_inside_c = corrected(t);
    g_inside_ms = millis();
  }
}

float inside_c() {
  // Symmetric with outside_c_fresh(): a reading nobody has been able to
  // refresh is not a reading. NaN is the contract fan_auto_decide already
  // handles ("hold speed AND latch: never guess").
  if (g_inside_ms == 0 || millis() - g_inside_ms > kInsideStaleMs)
    return NAN;
  return g_inside_c;
}
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
