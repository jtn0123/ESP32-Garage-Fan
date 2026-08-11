// See odometer.h. The counting and rollover logic is verbatim from the
// pre-split loop(); only the NVS flush moved inside.
#include "system/odometer.h"

#include <Arduino.h>

#include <ctime>

#include "system/timeutil.h"

namespace odometer {
namespace {

Preferences* g_prefs = nullptr;
uint32_t g_run_total_s = 0, g_run_today_s = 0;
uint32_t g_today_ymd = 0;
float g_energy_wh = 0;
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
}

void tick(int speed, float watts) {
  // Credit whole elapsed seconds rather than one per wakeup: a loop stalled
  // in an SD format for two minutes must not count as one second of runtime.
  const uint32_t elapsed_s = (millis() - g_last_count_ms) / 1000;
  if (elapsed_s >= 1) {
    g_last_count_ms += elapsed_s * 1000;

    // Roll the day BEFORE crediting, and split the elapsed span at the
    // boundary. This function deliberately credits whole accumulated seconds,
    // so one tick can cover minutes; crediting first and resetting afterwards
    // discarded the post-midnight portion along with the pre-midnight one, and
    // the new day silently started short.
    uint32_t today_s = elapsed_s;
    if (time_synced()) {
      struct tm lt;
      const time_t now = time(nullptr);
      // localtime_r, not gmtime_r: this is the OPERATOR's day, not UTC's.
      // With gmtime_r the counter labelled "FAN TODAY" in the console reset at
      // 17:00 local, so an evening glance showed only the hours since 5 PM --
      // during exactly the afternoon stretch the fan runs hardest. TZ is set
      // in wifi_link::begin(); the stored clock is still UTC.
      localtime_r(&now, &lt);
      const uint32_t ymd = (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
      if (g_today_ymd != 0 && ymd != g_today_ymd) {
        g_run_today_s = 0;
        // Seconds of this span that fall on the new day: everything since
        // local midnight, capped at the span itself.
        const uint32_t since_midnight = static_cast<uint32_t>(lt.tm_hour) * 3600U +
                                        static_cast<uint32_t>(lt.tm_min) * 60U +
                                        static_cast<uint32_t>(lt.tm_sec);
        today_s = since_midnight < elapsed_s ? since_midnight : elapsed_s;
      }
      g_today_ymd = ymd;
    }

    if (speed > 0) {
      g_run_total_s += elapsed_s;
      g_run_today_s += today_s;
      g_energy_wh += watts * elapsed_s / 3600.0f;
    }
  }
  if (g_prefs && millis() - g_last_nvs_ms >= 15 * 60 * 1000) {
    g_last_nvs_ms = millis();
    g_prefs->putUInt("runs", g_run_total_s);
    g_prefs->putUInt("runt", g_run_today_s);
    g_prefs->putUInt("ymd", g_today_ymd);
    g_prefs->putFloat("ewh", g_energy_wh);
  }
}

uint32_t run_today_s() { return g_run_today_s; }
uint32_t run_total_s() { return g_run_total_s; }
float energy_wh() { return g_energy_wh; }

}  // namespace odometer
