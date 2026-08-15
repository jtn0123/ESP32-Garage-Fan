#pragma once
// What the e-ink panel says, separated from how it is drawn.
//
// The panel is the only thing you can read with the garage door open and a
// phone in your pocket, so every string here is formatted for a glance: no
// units the reader has to infer, no blank where a missing reading should be
// named, and nothing that can spill past its box on the 250x122 panel.
//
// Pure so the host can test it. The drawing itself needs Adafruit_GFX and a
// panel; the DECISIONS -- what to show, how to word it, when something is
// stale, how wide the bar is -- do not, and those are what break.
//
// TRICOLOR RULE: red is an accent on top of information that is already legible
// in black, never the only carrier of it. docs/HARDWARE.md records this board's
// FeatherWing as the MONO SSD1680, and the tricolor part is the same driver at
// the same resolution -- so the same firmware drives both, and on a mono panel
// everything drawn red simply does not appear. If a fact were red-only it would
// silently vanish on half the hardware this code claims to support.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace display_layout {

// The 2.13" FeatherWing, landscape. Both the mono and tricolor parts are
// SSD1680 at this size.
constexpr int kWidth = 250;
constexpr int kHeight = 122;

/** The fan's top speed; the rail and the duty table both stop here. */
constexpr int kMaxSpeed = 12;

/**
 * Big centre-stage speed readout.
 *
 * "OFF" rather than "0": a stopped fan should not read as just another step,
 * the same reason the console's rail says off instead of 0.
 */
inline void speed_text(int speed, char* out, size_t cap) {
  if (!out || cap == 0)
    return;
  if (speed <= 0)
    snprintf(out, cap, "OFF");
  else
    snprintf(out, cap, "%d", speed);
}

/** The small "/12" that gives the big number its scale. Empty when off. */
inline void speed_scale_text(int speed, char* out, size_t cap) {
  if (!out || cap == 0)
    return;
  if (speed <= 0)
    out[0] = '\0';
  else
    snprintf(out, cap, "/%d", kMaxSpeed);
}

/**
 * How much of the speed bar is filled, 0..1.
 *
 * The bar is the red accent; the number beside it carries the same fact in
 * black, so a mono panel loses the flourish and none of the meaning.
 */
inline float speed_fraction(int speed) {
  if (speed <= 0)
    return 0.0f;
  if (speed >= kMaxSpeed)
    return 1.0f;
  return static_cast<float>(speed) / static_cast<float>(kMaxSpeed);
}

/**
 * A temperature in Fahrenheit, or an explicit placeholder.
 *
 * "--.-" keeps the glyph count stable so the layout does not jump between
 * refreshes, and it reads as "no reading" rather than as a real zero.
 */
inline void temp_f_text(float celsius, char* out, size_t cap) {
  if (!out || cap == 0)
    return;
  if (isnan(celsius))
    snprintf(out, cap, "--.-");
  else
    snprintf(out, cap, "%.1f", celsius * 9.0f / 5.0f + 32.0f);
}

/** Relative humidity, or a named absence. */
inline void rh_text(float rh, char* out, size_t cap) {
  if (!out || cap == 0)
    return;
  if (isnan(rh))
    snprintf(out, cap, "RH --");
  else
    snprintf(out, cap, "RH %.0f%%", rh);
}

/**
 * The differential the fan actually decides on: garage minus yard, in F.
 *
 * NAN when either side is missing -- the difference between two readings is
 * only meaningful when both exist, and showing 0.0 there would look like
 * "equalised", which is the exact state that turns the fan off.
 */
inline float differential_f(float inside_c, float outside_c) {
  if (isnan(inside_c) || isnan(outside_c))
    return NAN;
  return (inside_c - outside_c) * 9.0f / 5.0f;
}

inline void differential_text(float inside_c, float outside_c, char* out, size_t cap) {
  if (!out || cap == 0)
    return;
  const float d = differential_f(inside_c, outside_c);
  if (isnan(d))
    snprintf(out, cap, "DIFF --");
  else
    snprintf(out, cap, "DIFF %+.1f", d);
}

/**
 * Is the differential past the engage threshold?
 *
 * Drives the red accent on the differential line. The number is printed in
 * black either way.
 */
inline bool differential_is_hot(float inside_c, float outside_c, float on_f) {
  const float d = differential_f(inside_c, outside_c);
  return !isnan(d) && d >= on_f;
}

/** "AUTO" / "MANUAL" -- which one is deciding the speed right now. */
inline void mode_text(bool auto_on, char* out, size_t cap) {
  if (!out || cap == 0)
    return;
  snprintf(out, cap, auto_on ? "AUTO" : "MANUAL");
}

/**
 * The banner's right-hand slot: the mode, unless the watt meter is calling
 * the fan a liar (plug verdict -1), which outranks everything else the
 * panel could say -- it is the panel's whole reason to be glanced at.
 * Mono-safe by the tricolor rule: the banner text is black-plane on mono.
 */
inline void banner_status_text(bool auto_on, int plug_verdict, char* out, size_t cap) {
  if (!out || cap == 0)
    return;
  if (plug_verdict == -1)
    snprintf(out, cap, "FAN FAULT");
  else
    mode_text(auto_on, out, cap);
}

/**
 * Measured draw for the footer. Empty when there is no fresh reading -- the
 * footer simply omits it rather than showing a stale number as current.
 */
inline void power_text(float watts, char* out, size_t cap) {
  if (!out || cap == 0)
    return;
  if (isnan(watts) || watts < 0)
    out[0] = '\0';
  else
    snprintf(out, cap, "%.0fW", (double)watts);
}

/**
 * VOC index for the footer. 0 = Sensirion's algorithm still warming (worth
 * saying: a blank looks like a missing sensor); negative = no sensor, empty.
 */
inline void voc_text(int voc_index, char* out, size_t cap) {
  if (!out || cap == 0)
    return;
  if (voc_index < 0)
    out[0] = '\0';
  else if (voc_index == 0)
    snprintf(out, cap, "VOC WARM");
  else
    snprintf(out, cap, "VOC %d", voc_index);
}

/**
 * Runtime today as h/m, for the footer.
 *
 * Minutes stay two digits so the string does not change width every time the
 * clock ticks past a single-digit minute -- on e-ink a jittering footer is a
 * full refresh's worth of flashing for no new information.
 */
inline void runtime_text(uint32_t seconds, char* out, size_t cap) {
  if (!out || cap == 0)
    return;
  snprintf(out, cap, "%luh%02lum", (unsigned long)(seconds / 3600),
           (unsigned long)((seconds % 3600) / 60));
}

/** Battery volts and percent, or just volts when the gauge has no estimate. */
inline void battery_text(float volts, float percent, char* out, size_t cap) {
  if (!out || cap == 0)
    return;
  if (isnan(volts))
    snprintf(out, cap, "BAT --");
  else if (isnan(percent))
    snprintf(out, cap, "%.2fV", volts);
  else
    snprintf(out, cap, "%.2fV %.0f%%", volts, percent);
}

/**
 * Should the panel refresh?
 *
 * A full refresh parks the loop for seconds and flashes the whole panel, so it
 * happens on the sample cadence, not on every change. A speed change is worth
 * showing sooner, but still not immediately: a hand walking the rail from 1 to
 * 12 would otherwise strobe the panel twelve times and stall MQTT with it.
 *
 * `never_rendered` forces the first paint, so a fresh boot does not sit blank
 * for a whole sample period.
 */
inline bool refresh_due(uint32_t now_ms, uint32_t last_ms, uint32_t sample_ms, int speed_now,
                        int speed_shown, uint32_t speed_settle_ms, bool never_rendered) {
  if (never_rendered)
    return true;
  const uint32_t since = now_ms - last_ms;
  if (since >= sample_ms)
    return true;
  return speed_now != speed_shown && since >= speed_settle_ms;
}

}  // namespace display_layout
