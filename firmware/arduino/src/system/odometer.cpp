// See odometer.h. The counting and rollover logic is verbatim from the
// pre-split loop(); only the NVS flush moved inside.
#include "system/odometer.h"

#include <Arduino.h>

#include <ctime>

#include "system/odometer_logic.h"
#include "system/timeutil.h"

namespace odometer {
namespace {

Preferences* g_prefs = nullptr;
uint32_t g_run_total_s = 0, g_run_today_s = 0;
uint32_t g_today_ymd = 0;
float g_energy_wh = 0;
float g_wh_today = 0;      // rolls with the daily counter; measured watts when available
float g_cost_kwh = 0.15f;  // $/kWh; display math only, never billing
uint32_t g_last_count_ms = 0;
uint32_t g_last_nvs_ms = 0;

}  // namespace

void restore(Preferences* prefs) {
  g_prefs = prefs;
  if (!prefs)
    return;
  g_run_total_s = prefs->getUInt("runs", 0);
  g_run_today_s = prefs->getUInt("runt", 0);
  g_today_ymd = prefs->getUInt("ymd", 0);
  g_energy_wh = prefs->getFloat("ewh", 0);
  g_wh_today = prefs->getFloat("ewhd", 0);
  g_cost_kwh = prefs->getFloat("ckwh", 0.15f);
}

void tick(int speed, float watts) {
  // Credit whole elapsed seconds rather than one per wakeup: a loop stalled
  // in an SD format for two minutes must not count as one second of runtime.
  const uint32_t elapsed_s = (millis() - g_last_count_ms) / 1000;
  if (elapsed_s >= 1) {
    g_last_count_ms += elapsed_s * 1000;

    // The calendar reasoning lives in odometer_logic.h so it can be tested on
    // the host; see there for what each of the two bugs was.
    uint32_t ymd = 0, since_midnight = 0;
    const bool clock_valid = time_synced();
    if (clock_valid) {
      struct tm lt;
      const time_t now = time(nullptr);
      // localtime_r, not gmtime_r: this is the OPERATOR's day, not UTC's. TZ
      // is set in wifi_link::begin(); the stored clock is still UTC.
      localtime_r(&now, &lt);
      ymd = (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
      since_midnight = static_cast<uint32_t>(lt.tm_hour) * 3600U +
                       static_cast<uint32_t>(lt.tm_min) * 60U + static_cast<uint32_t>(lt.tm_sec);
    }
    const auto c =
        odometer_logic::credit(elapsed_s, speed > 0, clock_valid, ymd, g_today_ymd, since_midnight);
    if (c.reset_today) {
      g_run_today_s = 0;
      g_wh_today = 0;
    }
    g_run_total_s += c.total_s;
    g_run_today_s += c.today_s;
    if (speed > 0) {
      g_energy_wh += watts * elapsed_s / 3600.0f;
      // c.today_s, not elapsed_s: a tick that spans local midnight credits
      // only its post-midnight seconds to the new day -- same rule the
      // runtime counter already follows (odometer_logic::credit).
      g_wh_today += watts * c.today_s / 3600.0f;
    }
    if (clock_valid)
      g_today_ymd = ymd;
  }
  if (g_prefs && millis() - g_last_nvs_ms >= 15 * 60 * 1000) {
    g_last_nvs_ms = millis();
    g_prefs->putUInt("runs", g_run_total_s);
    g_prefs->putUInt("runt", g_run_today_s);
    g_prefs->putUInt("ymd", g_today_ymd);
    g_prefs->putFloat("ewh", g_energy_wh);
    g_prefs->putFloat("ewhd", g_wh_today);
  }
}

uint32_t run_today_s() { return g_run_today_s; }
uint32_t run_total_s() { return g_run_total_s; }
float energy_wh() { return g_energy_wh; }
float wh_today() { return g_wh_today; }

float cost_per_kwh() { return g_cost_kwh; }

void set_cost_per_kwh(float v) {
  g_cost_kwh = v;
  if (g_prefs)
    g_prefs->putFloat("ckwh", v);
}

}  // namespace odometer
