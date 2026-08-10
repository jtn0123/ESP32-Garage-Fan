// See display.h. Render body is verbatim from the pre-split firmware.
#include "ui/display.h"

#include <Adafruit_EPD.h>
#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <WiFi.h>

#include <cmath>

#include "config.h"
#include "fan/control.h"
#include "sensors/battery.h"
#include "sensors/climate.h"
#include "storage/history.h"
#include "system/crashlog.h"
#include "system/eventlog.h"
#include "system/odometer.h"

namespace display {
namespace {

// 2.13" mono SSD1680 FeatherWing, landscape; no busy/rst pins wired.
// Constructed lazily as a function-local static inside begin() so the driver
// object only exists once the panel is actually initialised.
Adafruit_SSD1680* g_epd = nullptr;
bool g_ok = false;
uint32_t g_last_ms = 0;
int g_speed_shown = -99;

}  // namespace

void begin() {
  CRUMB("epd_init");
  static Adafruit_SSD1680 epd(250, 122, EPD_DC_PIN, -1, EPD_CS_PIN, -1, -1);
  g_epd = &epd;
  g_epd->begin();
  g_epd->setRotation(1);
  g_ok = true;
  CRUMB_CLEAR();
}

void maybe_render() {
  if (!g_ok || !history::count())
    return;
  const bool due = (millis() - g_last_ms >= kSampleMs) ||
                   (fan::speed() != g_speed_shown && millis() - g_last_ms >= 60000);
  if (!due)
    return;
  // Stamp the cadence BEFORE the refresh, not after it. Stamping at the end
  // made the real period "every kSampleMs PLUS however long the panel took",
  // which is exactly why this device dropped its MQTT session every 317 s and
  // never on a round number: 300 s of waiting + ~17 s of blocking refresh
  // (2026-08-09).
  g_last_ms = millis();
  const uint32_t t_render = millis();
  CRUMB("epd");
  g_epd->clearBuffer();
  g_epd->fillScreen(EPD_WHITE);
  g_epd->setTextColor(EPD_BLACK);
  const float tin = history::count() ? history::temp()[history::count() - 1] : NAN;
  g_epd->setTextSize(4);
  g_epd->setCursor(4, 18);
  if (isnan(tin))
    g_epd->print("--.-");
  else
    g_epd->printf("%.1f", tin * 9 / 5 + 32);
  g_epd->setTextSize(1);
  g_epd->setCursor(6, 54);
  g_epd->print("GARAGE F");
  const float rh = history::count() ? history::rh()[history::count() - 1] : NAN;
  g_epd->setCursor(70, 54);
  if (!isnan(rh))
    g_epd->printf("RH %.0f%%", rh);
  g_epd->setTextSize(3);
  g_epd->setCursor(158, 8);
  if (fan::speed() <= 0)
    g_epd->print("OFF");
  else
    g_epd->printf("F%d", fan::speed());
  g_epd->setTextSize(1);
  g_epd->setCursor(158, 38);
  g_epd->print(fan::auto_on() ? "AUTO ON" : "AUTO OFF");
  const float oc = climate::outside_c_fresh();
  g_epd->setCursor(158, 52);
  if (isnan(oc))
    g_epd->print("OUT --");
  else
    g_epd->printf("OUT %.1fF", oc * 9 / 5 + 32);
  g_epd->setCursor(158, 66);
  if (!isnan(battery::volts())) {
    if (isnan(battery::percent()))
      g_epd->printf("BAT %.2fV --%%", battery::volts());
    else
      g_epd->printf("BAT %.2fV %.0f%%", battery::volts(), battery::percent());
  }
  g_epd->setCursor(6, 76);
  g_epd->printf("run today %luh%02lum  total %luh", (unsigned long)(odometer::run_today_s() / 3600),
                (unsigned long)(odometer::run_today_s() % 3600 / 60),
                (unsigned long)(odometer::run_total_s() / 3600));
  g_epd->setCursor(6, 106);
  g_epd->printf("%s  fw %s", WiFi.localIP().toString().c_str(), kFwVersion);
  g_epd->display();
  CRUMB_CLEAR();
  g_speed_shown = fan::speed();
  // Adafruit_EPD::display() is synchronous: it parks the entire loop while the
  // panel clocks its waveform out. Long enough that PubSubClient cannot send a
  // keepalive, which is what the broker was recording as "Client
  // garage-fan-03f784 disconnected: exceeded timeout" on a 317 s beat. Never
  // let that cost be invisible again -- the sample and the SD flush were both
  // instrumented and exonerated before anyone thought to time the screen.
  const uint32_t dt = millis() - t_render;
  if (dt > 2000)
    eventlog::log("slow", "epd refresh %lums", (unsigned long)dt);
}

}  // namespace display
