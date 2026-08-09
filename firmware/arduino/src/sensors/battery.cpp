// The backup pack's fuel gauge. See battery.h for why the charging verdict
// reaches well beyond the battery readout.
#include "sensors/battery.h"

#include <Adafruit_LC709203F.h>
#include <Adafruit_MAX1704X.h>
#include <Arduino.h>
#include <Wire.h>

#include <cmath>
#include <cstring>

#include "sensors/battery_logic.h"

namespace battery {
namespace {

Adafruit_MAX17048 g_max;
Adafruit_LC709203F g_lc;
Preferences* g_prefs = nullptr;
uint8_t g_kind = 0;
float g_volts = NAN, g_percent = NAN;
Window g_win;        // 5-min points; all the judgment calls live in battery_logic.h
bool g_chg = false;  // sticky charging verdict, NVS-persisted

}  // namespace

uint8_t kind() { return g_kind; }
float volts() { return g_volts; }
float percent() { return g_percent; }
bool charging() { return g_chg; }

void restore(Preferences* prefs) {
  g_prefs = prefs;
  if (prefs)
    g_chg = prefs->getBool("chg", false);
}

float slope_mv_per_h() { return g_win.slope_mv_per_h(g_volts); }

// Raw LC709203F access, bypassing the Adafruit library's begin() (whose IC
// version check fails on this board's chip even though it ACKs). CRC-8/ATM
// over the full I2C frame, per datasheet.
uint8_t crc8(const uint8_t* d, size_t n) {
  uint8_t crc = 0;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
  }
  return crc;
}

bool write_reg(uint8_t reg, uint16_t val) {
  uint8_t f[5] = {0x16, reg, (uint8_t)(val & 0xFF), (uint8_t)(val >> 8), 0};
  f[4] = crc8(f, 4);
  Wire.beginTransmission(0x0B);
  Wire.write(&f[1], 4);
  return Wire.endTransmission() == 0;
}

bool read_reg(uint8_t reg, uint16_t* out) {
  Wire.beginTransmission(0x0B);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom((uint8_t)0x0B, (uint8_t)3) != 3)
    return false;
  const uint8_t lo = Wire.read(), hi = Wire.read(), crc = Wire.read();
  const uint8_t chk[5] = {0x16, reg, 0x17, lo, hi};
  if (crc8(chk, 5) != crc)
    return false;
  *out = static_cast<uint16_t>(hi << 8) | lo;
  return true;
}

// Boards in this project carry either fuel gauge depending on Feather rev --
// probe both libraries, then fall back to raw LC register access (memory:
// LC709203F lives at 0x0B, MAX17048 at 0x36; scan before assuming).
void begin() {
  if (g_kind)
    return;
  // Raw LC access FIRST: this board's chip reports IC version 0x2AFF -- a
  // variant the Adafruit library rejects at begin() despite fully working
  // registers (proven 2026-08-05 via /api/battdebug: 3776 mV, 29% RSOC,
  // valid CRCs; requires repeated-start reads). The library path stays as
  // fallback for boards with recognized parts.
  uint16_t v = 0;
  if (write_reg(0x15, 0x0001) && write_reg(0x0B, 0x0055) &&  // 2x3200 = 6400 mAh
      read_reg(0x09, &v) && v > 2500 && v < 4600) {
    g_kind = 3;
    Serial.printf("fuel gauge: LC709203F-variant via raw access (%u mV)\n", v);
    return;
  }
  if (g_max.begin(&Wire)) {
    g_kind = 1;
    Serial.println("fuel gauge: MAX17048");
    return;
  }
  if (g_lc.begin(&Wire)) {
    g_lc.setPackAPA(0x56);  // dual-18650 6600 mAh pack, per the sensor node
    g_kind = 2;
    Serial.println("fuel gauge: LC709203F");
  }
}

void read() {
  if (!g_kind)
    return;
  float v = NAN, p = NAN;
  if (g_kind == 3) {
    uint16_t mv = 0, soc = 0;
    if (read_reg(0x09, &mv))
      v = mv / 1000.0f;
    if (read_reg(0x0D, &soc) && soc <= 100)
      p = soc;
  } else {
    v = g_kind == 1 ? g_max.cellVoltage() : g_lc.cellVoltage();
    p = g_kind == 1 ? g_max.cellPercent() : g_lc.cellPercent();
  }
  if (v > 2.0f && v < 5.0f) {
    g_volts = v;
    if (!isnan(p))
      g_percent = p > 100 ? 100 : p;
  }
}

// Re-judge the sticky charging verdict (battery_logic.h owns the thresholds
// and the why); this side persists a flip to NVS so a reboot mid-charge keeps
// the right temperature offset.
static void update_charging() {
  const bool next = judge_charging(g_win, g_volts, g_chg);
  if (next != g_chg) {
    g_chg = next;
    if (g_prefs)
      g_prefs->putBool("chg", g_chg);
    Serial.printf("charging: %s\n", g_chg ? "yes" : "no");
  }
}

// Discharge slope over the last hour of 5-min points -> hours remaining.
// Rising charge or a flat line means no meaningful ETA.
void eta(bool* charging, float* eta_h) {
  *charging = g_chg;
  *eta_h = eta_hours(g_win, g_percent, g_chg);
}

// One 5-minute history point, then re-judge the charging verdict. Lifted out of
// sample_climate() so the ring-buffer code no longer reaches into this
// module's sliding window.
void sample() {
  begin();
  read();
  if (isnan(g_volts))
    return;
  g_win.push(g_volts, g_percent);
  update_charging();
}

}  // namespace battery
