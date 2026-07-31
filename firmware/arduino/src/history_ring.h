#pragma once
// Copyright 2026 Justin
//
// 24-hour sample history for the on-device sparkline page. One slot per sample
// at the 5-minute cadence = 288 slots covering a day. RAM-only by design: the
// always-on node reboots rarely (OTA), and the ring refills within the day.
// Values are stored display-ready (temperatures in F, humidity in %); absent
// readings are NAN and render as gaps.
//
// Pure header (no Arduino includes) so a native test can exercise it directly.

#include <cmath>
#include <cstdint>

#define HIST_CAP 288

struct HistoryRing {
  float temp_in[HIST_CAP];
  float temp_out[HIST_CAP];
  float rh_in[HIST_CAP];
  float rh_out[HIST_CAP];
  uint16_t count = 0;  // valid samples (<= HIST_CAP)
  uint16_t head = 0;   // next write slot
};

inline HistoryRing& hist_ring() {
  static HistoryRing h;
  return h;
}

inline void hist_push(float temp_in_f, float temp_out_f, float rh_in_pct, float rh_out_pct) {
  HistoryRing& h = hist_ring();
  h.temp_in[h.head] = temp_in_f;
  h.temp_out[h.head] = temp_out_f;
  h.rh_in[h.head] = rh_in_pct;
  h.rh_out[h.head] = rh_out_pct;
  h.head = static_cast<uint16_t>((h.head + 1) % HIST_CAP);
  if (h.count < HIST_CAP)
    h.count++;
}

// i = 0 is the oldest retained sample, i = count-1 the newest.
inline float hist_at(const float* arr, uint16_t i) {
  const HistoryRing& h = hist_ring();
  uint16_t idx = static_cast<uint16_t>((h.head + HIST_CAP - h.count + i) % HIST_CAP);
  return arr[idx];
}

// Min/max across one or two series, skipping NaN. Returns false when no finite
// sample exists (renderers then skip the plot entirely).
inline bool hist_minmax(const float* a, const float* b, float* out_min, float* out_max) {
  const HistoryRing& h = hist_ring();
  float mn = INFINITY, mx = -INFINITY;
  for (uint16_t i = 0; i < h.count; i++) {
    const float va = hist_at(a, i);
    if (std::isfinite(va)) {
      if (va < mn)
        mn = va;
      if (va > mx)
        mx = va;
    }
    if (b) {
      const float vb = hist_at(b, i);
      if (std::isfinite(vb)) {
        if (vb < mn)
          mn = vb;
        if (vb > mx)
          mx = vb;
      }
    }
  }
  if (!std::isfinite(mn))
    return false;
  *out_min = mn;
  *out_max = (mx > mn) ? mx : mn + 1.0f;
  return true;
}
