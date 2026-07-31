#pragma once
// Copyright 2026 Justin
//
// Panel selection for the 2.13" eInk FeatherWing family. All of these wings
// share the FeatherWing pinout, so swapping the hardware is plug-and-play;
// the driver is a compile-time choice because e-ink is write-only and the
// attached panel cannot be probed at runtime.
//
//   (default)                   mono GDEY0213B74 / SSD1680   (Adafruit 4195)
//   -DEINK_PANEL_DEPG0213BN=1   mono DEPG0213BN              (older 4195 rev)
//   -DEINK_TRICOLOR=1           red/black/white Z98c / SSD1680 (Adafruit 4814)
//
// Tri-color reality check (GxEPD2_213_Z98c): hasFastPartialUpdate is false
// and every refresh - full or "partial" - is a ~15 s flash cycle. The
// always-on loop therefore skips its flash-free per-sample partial update on
// that panel (see kDisplayHasFastPartial) and repaints on the full-refresh
// cadence only. Red is an accent: spec ops with color:"red" render red on
// the tri-color wing and fall back to black on the mono panels (GxEPD2_BW
// itself would render any non-black color as WHITE, so the fallback must be
// mapped here, never left to the driver).

#include <GxEPD2_3C.h>
#include <GxEPD2_BW.h>

#ifndef EINK_TRICOLOR
#define EINK_TRICOLOR 0
#endif
#ifndef EINK_PANEL_DEPG0213BN
#define EINK_PANEL_DEPG0213BN 0
#endif

#if EINK_TRICOLOR
using DisplayPanel = GxEPD2_213_Z98c;
using DisplayType = GxEPD2_3C<GxEPD2_213_Z98c, GxEPD2_213_Z98c::HEIGHT>;
#elif EINK_PANEL_DEPG0213BN
using DisplayPanel = GxEPD2_213_DEPG0213BN;
using DisplayType = GxEPD2_BW<GxEPD2_213_DEPG0213BN, GxEPD2_213_DEPG0213BN::HEIGHT>;
#else
using DisplayPanel = GxEPD2_213_GDEY0213B74;
using DisplayType = GxEPD2_BW<GxEPD2_213_GDEY0213B74, GxEPD2_213_GDEY0213B74::HEIGHT>;
#endif

extern DisplayType display;

// True when the panel supports flash-free partial refresh (the live
// per-sample update path). False on tri-color: its "partial" is a full
// 15 s wave, which would make every 5-minute sample a screen-wide flash.
constexpr bool kDisplayHasFastPartial = DisplayPanel::hasFastPartialUpdate;

// Spec op color codes as emitted by gen_ui.py: 0 black, 1 white, 2 red.
inline uint16_t map_op_color(int code) {
  if (code == 1)
    return GxEPD_WHITE;
  if (code == 2)
#if EINK_TRICOLOR
    return GxEPD_RED;
#else
    return GxEPD_BLACK;  // mono fallback: accents render as ink
#endif
  return GxEPD_BLACK;
}
