// See display.h. What to say lives in display_layout.h; this is how it is drawn
// and how the same pixels reach both the panel and the web console.
#include "ui/display.h"

#include <Adafruit_EPD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ThinkInk.h>
#include <Arduino.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <WiFi.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "fan/control.h"
#include "net/plug.h"
#include "sensors/air.h"
#include "sensors/battery.h"
#include "sensors/climate.h"
#include "storage/history.h"
#include "system/crashlog.h"
#include "system/eventlog.h"
#include "system/odometer.h"
#include "ui/display_layout.h"
#include "ui/qr_v1.h"

namespace display {
namespace {

using display_layout::banner_status_text;
using display_layout::battery_text;
using display_layout::differential_is_hot;
using display_layout::differential_text;
using display_layout::kHeight;
using display_layout::kWidth;
using display_layout::power_text;
using display_layout::refresh_due;
using display_layout::rh_text;
using display_layout::runtime_text;
using display_layout::speed_fraction;
using display_layout::speed_scale_text;
using display_layout::speed_text;
using display_layout::temp_f_text;
using display_layout::voc_text;

// 2.13" SSD1680 FeatherWing, landscape; no busy/rst pins wired. Driven through
// Adafruit's own panel class for the tricolor MFGNR part: the raw SSD1680
// default keeps _xram_offset = 1, which shifted the whole image 8 px down this
// glass -- a band of never-written RAM speckling red above the title, and the
// footer's bottom rows pushed off the panel (observed 2026-08-12). The MFGNR
// class zeroes the offset and carries the right refresh timing.
/**
 * The stock Adafruit_EPD::display() is synchronous: after kicking the panel
 * it delay()s default_refresh_delay (13 s here, no busy pin wired) with the
 * whole loop parked -- no HTTP, no MQTT keepalive, no auto tick, ~5% of the
 * device's life frozen. But the SSD1680 refreshes AUTONOMOUSLY once
 * MASTER_ACTIVATE lands: it clocks the waveform from its own RAM with CS
 * high, and the shared SPI bus is free for the SD card the whole time. So
 * this subclass splits display() at exactly that line: start_refresh() does
 * everything up to and including the kick (~0.3 s of real SPI work), and
 * finish_refresh() powers the panel down after the loop has waited out the
 * window ASYNCHRONOUSLY (see maybe_render). The panel must simply not be
 * spoken to in between.
 */
class AsyncEink : public ThinkInk_213_Tricolor_MFGNR {
 public:
  using ThinkInk_213_Tricolor_MFGNR::ThinkInk_213_Tricolor_MFGNR;

  void start_refresh() {
    powerUp();
    setRAMAddress(0, 0);
    writeRAMFramebufferToEPD(buffer1, buffer1_size, 0);
    if (buffer2_size != 0) {
      delay(2);
      setRAMAddress(0, 0);
      writeRAMFramebufferToEPD(buffer2, buffer2_size, 1);
    }
    uint8_t buf[1] = {_display_update_val};
    EPD_command(SSD1680_DISP_CTRL2, buf, 1);
    EPD_command(SSD1680_MASTER_ACTIVATE);
    // NO wait: the panel is now refreshing itself from its own RAM.
  }

  void finish_refresh() { powerDown(); }
};

AsyncEink* g_epd = nullptr;
bool g_ok = false;
// The refresh window. default_refresh_delay is 13 s for this glass and the
// full blocking path measured 14.74 s; 16 s clears both with margin. While
// g_refreshing, the panel owns itself and this module only watches the clock.
constexpr uint32_t kRefreshHoldMs = 16000;
bool g_refreshing = false;
uint32_t g_refresh_kicked_ms = 0;
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

/**
 * Floor between FORCED refreshes, for the console's "Refresh now".
 *
 * A refresh blocks the loop for seconds (16.7 s measured on this panel), so
 * back-to-back forced repaints are a denial of service against the fan's own
 * control loop and its broker session. Longer than one refresh by a wide
 * margin, short enough that the button is still worth pressing.
 */
constexpr uint32_t kMinForcedGapMs = 30000;

/** Built-in 5x7 font, (x, y) = top-left. For labels and the footer. */
void draw_text(GFXcanvas1* c, int x, int y, uint8_t size, const char* s) {
  c->setFont(nullptr);
  c->setTextSize(size);
  c->setCursor(x, y);
  c->print(s);
}

/** A FreeFont, (x, y) = BASELINE, like every GFX custom font. */
void draw_ftext(GFXcanvas1* c, const GFXfont* f, int x, int y, const char* s, uint16_t color = 1) {
  c->setFont(f);
  c->setTextSize(1);
  c->setTextColor(color);
  c->setCursor(x, y);
  c->print(s);
  c->setTextColor(1);
  c->setFont(nullptr);
}

/** Advance width of `s` in font `f`, for right-aligning. */
int ftext_width(GFXcanvas1* c, const GFXfont* f, const char* s) {
  int16_t x1;
  int16_t y1;
  uint16_t w;
  uint16_t h;
  c->setFont(f);
  c->setTextSize(1);
  c->getTextBounds(s, 0, 100, &x1, &y1, &w, &h);
  c->setFont(nullptr);
  return static_cast<int>(w) + x1;
}

/**
 * Compose one frame into the two planes.
 *
 * Layout, 250x122 landscape ("the mix", chosen over A/B/C previews 2026-08-12):
 *   banner   : solid red band, title and mode knocked out to paper
 *   left col : IN temp (FreeSansBold 18pt), RH, OUT temp (12pt)
 *   right col: FAN speed (24pt), /12, red fill bar, differential
 *   footer   : runtime, battery, ip, fw (built-in 5x7)
 *
 * Every element lives in exactly ONE plane -- a pixel set in both RAMs gets an
 * ill-defined waveform and speckles on this chemistry. The knockout title is
 * still one plane: it is holes in the red band, not black over red.
 */
void compose() {
  char buf[48];
  g_black->fillScreen(0);
  g_red->fillScreen(0);

  const float tin = climate::inside_c();
  const float tout = climate::outside_c_fresh();
  const float rh = history::count() ? history::rh()[history::count() - 1] : NAN;
  const int speed = fan::speed();

  // ---- banner ---------------------------------------------------------------
  // "FAN FAULT" outranks the mode: it is the watt meter catching the fan not
  // obeying (plug verdict -1), the one condition this panel exists to shout.
  banner_status_text(fan::auto_on(), plug::verdict(), plug::cycling(), buf, sizeof(buf));
  if (tricolor()) {
    g_red->fillRect(0, 0, kWidth, 20, 1);
    draw_ftext(g_red, &FreeSansBold9pt7b, 5, 15, "GARAGE FAN", 0);
    draw_ftext(g_red, &FreeSansBold9pt7b, kWidth - 5 - ftext_width(g_red, &FreeSansBold9pt7b, buf),
               15, buf, 0);
  } else {
    draw_ftext(g_black, &FreeSansBold9pt7b, 5, 15, "GARAGE FAN");
    draw_ftext(g_black, &FreeSansBold9pt7b,
               kWidth - 5 - ftext_width(g_black, &FreeSansBold9pt7b, buf), 15, buf);
    g_black->drawFastHLine(0, 19, kWidth, 1);
  }

  // ---- left column: the two temperatures ------------------------------------
  draw_text(g_black, 4, 27, 1, "IN");
  temp_f_text(tin, buf, sizeof(buf));
  draw_ftext(g_black, &FreeSansBold18pt7b, 2, 60, buf);
  draw_ftext(g_black, &FreeSansBold9pt7b, g_black->getCursorX() + 2, 47, "F");
  rh_text(rh, buf, sizeof(buf));
  draw_text(g_black, 4, 66, 1, buf);

  draw_text(g_black, 4, 78, 1, "OUT");
  temp_f_text(tout, buf, sizeof(buf));
  draw_ftext(g_black, &FreeSansBold12pt7b, 2, 103, buf);
  draw_ftext(g_black, &FreeSansBold9pt7b, g_black->getCursorX() + 2, 94, "F");

  // ---- right column: the fan -------------------------------------------------
  constexpr int kRx = 132;
  g_black->drawFastVLine(kRx - 8, 24, 78, 1);
  draw_text(g_black, kRx + 2, 27, 1, "FAN");
  speed_text(speed, buf, sizeof(buf));
  // 24pt only for a single digit: "OFF" and two-digit speeds at 24pt would
  // run "/12" into the QR code on the right.
  draw_ftext(g_black, (speed >= 1 && speed <= 9) ? &FreeSansBold24pt7b : &FreeSansBold18pt7b, kRx,
             68, buf);
  speed_scale_text(speed, buf, sizeof(buf));
  if (buf[0])
    draw_ftext(g_black, &FreeSansBold9pt7b, g_black->getCursorX() + 4, 68, buf);

  // The console link as a V1 QR, 2 px per module (8 mm on this glass), in the
  // dead space right of the speed. Skipped before WiFi has an address: a code
  // that scans to 0.0.0.0 is worse than none. Quiet zone is the paper around
  // it -- 6 px to the banner, 4 px to the edge, both >= the 2-module minimum.
  const IPAddress ip = WiFi.localIP();
  if (ip != IPAddress()) {
    char url[32];
    snprintf(url, sizeof(url), "HTTP://%s", ip.toString().c_str());
    uint8_t qr[qr_v1::kSize][qr_v1::kSize];
    if (qr_v1::encode(url, qr)) {
      constexpr int kQx = 204;
      constexpr int kQy = 25;
      for (int qy = 0; qy < qr_v1::kSize; ++qy)
        for (int qx = 0; qx < qr_v1::kSize; ++qx)
          if (qr[qy][qx])
            g_black->fillRect(kQx + qx * 2, kQy + qy * 2, 2, 2, 1);
    }
  }

  // The bar: black outline, red fill strictly inside it -- one plane each.
  // Everything it says is also in the number above, so mono loses only colour.
  constexpr int kBarX = kRx;
  constexpr int kBarY = 76;
  constexpr int kBarW = kWidth - kRx - 6;
  constexpr int kBarH = 12;
  g_black->drawRect(kBarX, kBarY, kBarW, kBarH, 1);
  const int fill = static_cast<int>(speed_fraction(speed) * (kBarW - 2) + 0.5f);
  if (fill > 0) {
    if (tricolor()) {
      g_red->fillRect(kBarX + 1, kBarY + 1, fill, kBarH - 2, 1);
    } else {
      // Hatch so a mono panel still shows extent, not a blank box.
      for (int x = kBarX + 1; x < kBarX + 1 + fill; x += 3)
        g_black->drawFastVLine(x, kBarY + 1, kBarH - 2, 1);
    }
  }

  differential_text(tin, tout, buf, sizeof(buf));  // "DIFF +8.2", label included
  draw_ftext(g_black, &FreeSansBold9pt7b, kRx + 2, 103, buf);
  if (differential_is_hot(tin, tout, fan::engage_f())) {
    // A caret beside the number, not instead of it. One plane each.
    if (tricolor())
      g_red->fillTriangle(kWidth - 14, 102, kWidth - 6, 102, kWidth - 10, 94, 1);
    else
      g_black->drawTriangle(kWidth - 14, 102, kWidth - 6, 102, kWidth - 10, 94, 1);
  }

  // ---- footer -------------------------------------------------------------
  g_black->drawFastHLine(0, 106, kWidth, 1);
  // Sized so the concatenation below is provably in range: 15 + 2 + 23 < 64.
  // Reusing the shared buf[48] here left gcc unable to bound the first %s and
  // it warned about truncation -- and this build treats a warning in src/ as an
  // error, correctly, because a silently clipped footer is a real defect.
  char rt[16];
  char batt[24];
  char pw[12];
  char voc[16];
  char line[80];
  runtime_text(odometer::run_today_s(), rt, sizeof(rt));
  battery_text(battery::volts(), battery::percent(), batt, sizeof(batt));
  // Measured draw and air quality earn footer space only when they have
  // something true to say: power_text is empty without a fresh reading,
  // voc_text is empty without a sensor (and says WARM while warming).
  const bool plug_fresh = plug::age_s() >= 0 && plug::age_s() < 60;
  power_text(plug_fresh ? plug::watts() : NAN, pw, sizeof(pw));
  voc_text(static_cast<int>(air::voc_index()), voc, sizeof(voc));
  // The IP/version block is right-aligned on the SAME row, so the left side
  // must budget for it: 5x7 font at size 1 is 6 px per glyph, and the two
  // optional fields are appended only while they still fit inside the budget
  // (watts first -- it is the rarer, more actionable number). Without this,
  // "15h56m  4.19V 100%  20W  VOC 156" ran straight through the address.
  char right[40];
  snprintf(right, sizeof(right), "%s v%s", WiFi.localIP().toString().c_str(), kFwVersion);
  const size_t budget =
      static_cast<size_t>((kWidth - 4 - static_cast<int>(strlen(right)) * 6 - 4 - 8) / 6);
  snprintf(line, sizeof(line), "%s  %s", rt, batt);
  if (pw[0] && strlen(line) + 2 + strlen(pw) <= budget) {
    strlcat(line, "  ", sizeof(line));
    strlcat(line, pw, sizeof(line));
  }
  if (voc[0] && strlen(line) + 2 + strlen(voc) <= budget) {
    strlcat(line, "  ", sizeof(line));
    strlcat(line, voc, sizeof(line));
  }
  draw_text(g_black, 4, 108, 1, line);
  draw_text(g_black, kWidth - 4 - static_cast<int>(strlen(right)) * 6, 108, 1, right);
}

/** Push both planes to the panel. */
void blit() {
  g_epd->clearBuffer();
  g_epd->fillScreen(EPD_WHITE);
  g_epd->drawBitmap(0, 0, g_black->getBuffer(), kWidth, kHeight, EPD_BLACK);
  // The red plane only goes to the glass that can show it. On the mono part
  // RAM2 is not a colour plane, so writing accents there invites artifacts.
  if (tricolor())
    g_epd->drawBitmap(0, 0, g_red->getBuffer(), kWidth, kHeight, EPD_RED);
  g_epd->start_refresh();
  g_refreshing = true;
  g_refresh_kicked_ms = millis();
}

}  // namespace

void begin() {
  CRUMB("epd_init");
  // The MFGNR panel class, not raw Adafruit_SSD1680: it zeroes _xram_offset
  // (the raw default of 1 shifted the image 8 px down this glass, leaving a
  // never-written speckle band above the title and clipping the footer) and
  // its begin() sets rotation 0, which IS upright landscape on this wing.
  static AsyncEink epd(EPD_DC_PIN, -1, EPD_CS_PIN, -1, -1);
  g_epd = &epd;
  epd.begin(tricolor() ? THINKINK_TRICOLOR : THINKINK_MONO);
  static GFXcanvas1 black(kWidth, kHeight);
  static GFXcanvas1 red(kWidth, kHeight);
  g_black = &black;
  g_red = &red;
  g_ok = g_black->getBuffer() != nullptr && g_red->getBuffer() != nullptr;
  if (!g_ok)
    eventlog::log("epd", "no memory for the frame planes; panel disabled");
  CRUMB_CLEAR();
}

bool request_refresh() {
  // Rate-limited, and the caller is told when it was refused.
  //
  // A forced refresh bypasses refresh_due entirely, and the route that reaches
  // this is an unauthenticated LAN POST. One full refresh parks the loop long
  // enough that PubSubClient cannot send a keepalive -- this device already
  // lost its MQTT session to exactly that, on a 317 s beat, and the panel has
  // been measured at 16.7 s per refresh on this hardware. A client posting in a
  // loop would keep the loop parked and the glass cycling indefinitely.
  //
  // The automatic path is rate-limited by refresh_due for the same reason; this
  // gives the manual path the same floor. It lives here, not in web.cpp,
  // because this module owns g_last_ms.
  if (g_rendered && millis() - g_last_ms < kMinForcedGapMs)
    return false;
  g_force = true;
  return true;
}

uint32_t forced_retry_in_s() {
  if (!g_rendered)
    return 0;
  const uint32_t since = millis() - g_last_ms;
  return since >= kMinForcedGapMs ? 0 : (kMinForcedGapMs - since + 999) / 1000;
}

void maybe_render() {
  if (!g_ok)
    return;
  // A refresh in flight owns the panel. The loop keeps running -- that is
  // the whole point -- and this module only checks the clock: inside the
  // window nothing may talk to the glass, at its end the panel is powered
  // down and normal service resumes. g_force survives the window and is
  // honoured on the next pass.
  if (g_refreshing) {
    if (millis() - g_refresh_kicked_ms < kRefreshHoldMs)
      return;
    g_epd->finish_refresh();
    g_refreshing = false;
  }
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
  // Only the BLOCKING portion is timed now: compose + RAM writes + the kick,
  // ~0.3 s. The 13-14 s waveform runs asynchronously (see AsyncEink) with the
  // loop live, which is what ended the era of this refresh parking MQTT
  // keepalives and HTTP for 14.74 s at a time ("Client garage-fan-03f784
  // disconnected: exceeded timeout" on a 317 s beat, 2026-08-09). Anything
  // over 2 s here means the SPI write path itself is sick.
  const uint32_t dt = millis() - t_render;
  if (dt > 2000)
    eventlog::log("slow", "epd kick %lums", (unsigned long)dt);
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
