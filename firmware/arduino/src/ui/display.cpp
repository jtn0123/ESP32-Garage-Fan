// See display.h. What to say lives in display_layout.h; this is how it is drawn
// and how the same pixels reach both the panel and the web console.
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
#include "ui/display_layout.h"

namespace display {
namespace {

using namespace display_layout;

// 2.13" SSD1680 FeatherWing, landscape; no busy/rst pins wired. The mono and
// tricolor parts are the same driver at the same size, so one build covers both
// -- see display.h::tricolor() and the TRICOLOR RULE in display_layout.h.
Adafruit_SSD1680* g_epd = nullptr;
bool g_ok = false;
uint32_t g_last_ms = 0;
bool g_rendered = false;
int g_speed_shown = -99;
bool g_force = false;

// Our own planes, drawn first and then blitted. GFXcanvas1 is row-major, MSB
// first, set bit = ink -- a format we can serve and test, unlike the driver's.
// ~3.9 KB each. Allocated in begin(), which runs before WiFi, when the heap
// still has large contiguous blocks to give.
GFXcanvas1* g_black = nullptr;
GFXcanvas1* g_red = nullptr;

/** A speed change waits this long before earning a refresh. See refresh_due. */
constexpr uint32_t kSpeedSettleMs = 60000;

void draw_text(GFXcanvas1* c, int x, int y, uint8_t size, const char* s) {
  c->setTextSize(size);
  c->setCursor(x, y);
  c->print(s);
}

/**
 * Compose one frame into the two planes.
 *
 * Layout, 250x122 landscape:
 *   header   : name, mode, red rule
 *   left col : IN temp (big), RH, OUT temp
 *   right col: FAN speed (big), /12, red fill bar, differential
 *   footer   : runtime, battery, ip, fw
 */
void compose() {
  char buf[48];
  g_black->fillScreen(0);
  g_red->fillScreen(0);

  const float tin = climate::inside_c();
  const float tout = climate::outside_c_fresh();
  const float rh = history::count() ? history::rh()[history::count() - 1] : NAN;
  const int speed = fan::speed();

  // ---- header -------------------------------------------------------------
  draw_text(g_black, 4, 4, 1, "GARAGE FAN");
  mode_text(fan::auto_on(), buf, sizeof(buf));
  // Right-aligned by hand: 6 px per character at size 1.
  draw_text(g_black, kWidth - 4 - static_cast<int>(strlen(buf)) * 6, 4, 1, buf);
  // The rule is pure decoration, so it is the one thing drawn red-only.
  g_red->fillRect(0, 14, kWidth, 2, 1);
  g_black->drawFastHLine(0, 15, kWidth, 1);

  // ---- left column: the two temperatures ----------------------------------
  draw_text(g_black, 4, 22, 1, "IN F");
  temp_f_text(tin, buf, sizeof(buf));
  draw_text(g_black, 4, 34, 3, buf);
  rh_text(rh, buf, sizeof(buf));
  draw_text(g_black, 4, 60, 1, buf);

  draw_text(g_black, 4, 76, 1, "OUT F");
  temp_f_text(tout, buf, sizeof(buf));
  draw_text(g_black, 4, 88, 2, buf);

  // ---- right column: the fan ----------------------------------------------
  constexpr int kRx = 132;
  g_black->drawFastVLine(kRx - 8, 20, 82, 1);
  draw_text(g_black, kRx, 22, 1, "FAN");
  speed_text(speed, buf, sizeof(buf));
  draw_text(g_black, kRx, 34, speed <= 0 ? 3 : 5, buf);
  speed_scale_text(speed, buf, sizeof(buf));
  if (buf[0])
    draw_text(g_black, kRx + 40, 52, 2, buf);

  // The bar: red fill, black outline and a black tick at each step. Everything
  // it says is also in the number above it, so a mono panel loses only colour.
  constexpr int kBarX = kRx;
  constexpr int kBarY = 72;
  constexpr int kBarW = kWidth - kRx - 6;
  constexpr int kBarH = 10;
  g_black->drawRect(kBarX, kBarY, kBarW, kBarH, 1);
  const int fill = static_cast<int>(speed_fraction(speed) * (kBarW - 2) + 0.5f);
  if (fill > 0) {
    g_red->fillRect(kBarX + 1, kBarY + 1, fill, kBarH - 2, 1);
    // Hatch inside the fill so a mono panel still shows extent, not a blank box.
    for (int x = kBarX + 1; x < kBarX + 1 + fill; x += 3)
      g_black->drawFastVLine(x, kBarY + 1, kBarH - 2, 1);
  }

  differential_text(tin, tout, buf, sizeof(buf));
  draw_text(g_black, kRx, 88, 1, buf);
  if (differential_is_hot(tin, tout, fan::engage_f())) {
    // A caret beside the number, not instead of it.
    g_red->fillTriangle(kWidth - 14, 94, kWidth - 8, 94, kWidth - 11, 88, 1);
    g_black->drawTriangle(kWidth - 14, 94, kWidth - 8, 94, kWidth - 11, 88, 1);
  }

  // ---- footer -------------------------------------------------------------
  g_black->drawFastHLine(0, 104, kWidth, 1);
  runtime_text(odometer::run_today_s(), buf, sizeof(buf));
  char line[64];
  char batt[24];
  battery_text(battery::volts(), battery::percent(), batt, sizeof(batt));
  snprintf(line, sizeof(line), "%s  %s", buf, batt);
  draw_text(g_black, 4, 108, 1, line);
  snprintf(line, sizeof(line), "%s v%s", WiFi.localIP().toString().c_str(), kFwVersion);
  draw_text(g_black, kWidth - 4 - static_cast<int>(strlen(line)) * 6, 108, 1, line);
}

/** Push both planes to the panel. */
void blit() {
  g_epd->clearBuffer();
  g_epd->fillScreen(EPD_WHITE);
  g_epd->drawBitmap(0, 0, g_black->getBuffer(), kWidth, kHeight, EPD_BLACK);
  g_epd->drawBitmap(0, 0, g_red->getBuffer(), kWidth, kHeight, EPD_RED);
  g_epd->display();
}

}  // namespace

void begin() {
  CRUMB("epd_init");
  static Adafruit_SSD1680 epd(kWidth, kHeight, EPD_DC_PIN, -1, EPD_CS_PIN, -1, -1);
  g_epd = &epd;
  g_epd->begin();
  g_epd->setRotation(1);
  static GFXcanvas1 black(kWidth, kHeight);
  static GFXcanvas1 red(kWidth, kHeight);
  g_black = &black;
  g_red = &red;
  g_ok = g_black->getBuffer() != nullptr && g_red->getBuffer() != nullptr;
  if (!g_ok)
    eventlog::log("epd", "no memory for the frame planes; panel disabled");
  CRUMB_CLEAR();
}

void request_refresh() { g_force = true; }

void maybe_render() {
  if (!g_ok)
    return;
  const bool due = g_force || refresh_due(millis(), g_last_ms, kSampleMs, fan::speed(),
                                          g_speed_shown, kSpeedSettleMs, !g_rendered);
  if (!due)
    return;
  g_force = false;
  // Stamp the cadence BEFORE the refresh, not after it. Stamping at the end
  // made the real period "every kSampleMs PLUS however long the panel took",
  // which is exactly why this device dropped its MQTT session every 317 s and
  // never on a round number: 300 s of waiting + ~17 s of blocking refresh
  // (2026-08-09).
  g_last_ms = millis();
  const uint32_t t_render = millis();
  CRUMB("epd");
  compose();
  blit();
  CRUMB_CLEAR();
  g_rendered = true;
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

const uint8_t* plane_black() { return g_rendered && g_black ? g_black->getBuffer() : nullptr; }
const uint8_t* plane_red() { return g_rendered && g_red ? g_red->getBuffer() : nullptr; }
uint16_t width() { return kWidth; }
uint16_t height() { return kHeight; }
uint16_t stride() { return (kWidth + 7) / 8; }
size_t plane_bytes() { return static_cast<size_t>(stride()) * kHeight; }

int32_t age_s() {
  if (!g_rendered)
    return -1;
  return static_cast<int32_t>((millis() - g_last_ms) / 1000);
}

bool tricolor() { return FAN_EPD_TRICOLOR != 0; }

}  // namespace display
