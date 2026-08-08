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
  if (millis() - g_last_count_ms >= 1000) {
    g_last_count_ms = millis();
    if (speed > 0) {
      g_run_total_s++;
      g_run_today_s++;
      g_energy_wh += watts / 3600.0f;
    }
    if (time_synced()) {
      struct tm lt;
      time_t now = time(nullptr);
      gmtime_r(&now, &lt);
      const uint32_t ymd = (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
      if (g_today_ymd != 0 && ymd != g_today_ymd)
        g_run_today_s = 0;
      g_today_ymd = ymd;
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
