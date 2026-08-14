#pragma once
// The parallel-array sample ring behind storage/history, kept free of Arduino
// and config includes so the native_storage_ring env can exercise fill, wrap
// and the stats fold on the host. history.cpp instantiates one Ring<kRingLen>;
// tests instantiate a small one so wraparound is cheap to reach.
//
// Parallel arrays rather than an array of structs on purpose: the JSON
// writers iterate one column at a time, and a column pointer straight into
// the ring is the whole accessor story (see history.h).

#include <stdint.h>

#include <cmath>
#include <cstring>

namespace history {

struct Sample {
  float t;         // garage temperature, C (SHT41 when fitted, else corrected BME280)
  float h;         // %RH
  float p;         // hPa (always the BME280; nothing else has a barometer)
  float out_f;     // yard, F (NAN = no fresh feed at sample time)
  float batt_v;    // NAN = no fuel gauge
  float watts;     // fan draw measured at the plug (NAN = no meter reading)
  int32_t voc_raw; // SGP41 raw ticks (-1 = no sensor)
  int32_t nox_raw; // SGP41 raw ticks (-1 = no sensor)
  int16_t voc;     // VOC gas index 1..500, 0 = algorithm warming, -1 = absent
  int16_t nox;     // NOx gas index, same encoding
  int8_t speed;    // fan speed 0..12 at sample time
  int8_t chg;      // 1 charging, 0 not, -1 no battery
};

template <uint16_t kN>
struct Ring {
  float t[kN], h[kN], p[kN];
  float o[kN];      // outdoor F at sample time (NAN = none)
  float bv[kN];     // battery volts at sample time (NAN = none)
  float w[kN];      // fan draw at the plug (NAN = none)
  int32_t vr[kN];   // SGP41 VOC raw ticks (-1 = none)
  int32_t nr[kN];   // SGP41 NOx raw ticks (-1 = none)
  int16_t vi[kN];   // VOC index (0 = warming, -1 = none)
  int16_t ni[kN];   // NOx index, same encoding
  int8_t s[kN];     // fan speed at sample time
  int8_t c[kN];     // charging verdict (1/0, -1 = no battery)
  uint16_t n = 0;

  /** Append a row, evicting the oldest once full. */
  void append(const Sample& row) {
    if (n == kN) {
      memmove(t, t + 1, (kN - 1) * sizeof(float));
      memmove(h, h + 1, (kN - 1) * sizeof(float));
      memmove(p, p + 1, (kN - 1) * sizeof(float));
      memmove(o, o + 1, (kN - 1) * sizeof(float));
      memmove(s, s + 1, (kN - 1) * sizeof(int8_t));
      memmove(bv, bv + 1, (kN - 1) * sizeof(float));
      memmove(w, w + 1, (kN - 1) * sizeof(float));
      memmove(vr, vr + 1, (kN - 1) * sizeof(int32_t));
      memmove(nr, nr + 1, (kN - 1) * sizeof(int32_t));
      memmove(vi, vi + 1, (kN - 1) * sizeof(int16_t));
      memmove(ni, ni + 1, (kN - 1) * sizeof(int16_t));
      memmove(c, c + 1, (kN - 1) * sizeof(int8_t));
      n--;
    }
    t[n] = row.t;
    h[n] = row.h;
    p[n] = row.p;
    o[n] = row.out_f;
    s[n] = row.speed;
    bv[n] = row.batt_v;
    w[n] = row.watts;
    vr[n] = row.voc_raw;
    nr[n] = row.nox_raw;
    vi[n] = row.voc;
    ni[n] = row.nox;
    c[n] = row.chg;
    n++;
  }

  /** Min/max/avg of the temperature column; NANs when the ring is empty. */
  void temp_stats(float* mn, float* mx, float* avg) const {
    *mn = *mx = *avg = NAN;
    if (!n)
      return;
    float sum = 0;
    for (uint16_t i = 0; i < n; i++) {
      const float ti = t[i];
      if (std::isnan(*mn) || ti < *mn)
        *mn = ti;
      if (std::isnan(*mx) || ti > *mx)
        *mx = ti;
      sum += ti;
    }
    *avg = sum / n;
  }
};

}  // namespace history
