// See history.h.
#include "storage/history.h"

#include <cmath>
#include <cstring>

#include "config.h"

namespace history {
namespace {

float g_t[kRingLen], g_h[kRingLen], g_p[kRingLen];
float g_o[kRingLen];   // outdoor F at sample time (NAN = none)
int8_t g_s[kRingLen];  // fan speed at sample time
float g_bv[kRingLen];  // battery volts at sample time (NAN = none)
int8_t g_c[kRingLen];  // charging verdict (1/0, -1 = no battery)
time_t g_end_ts = 0;
uint16_t g_count = 0;

}  // namespace

void append(const Sample& s, bool stamped) {
  if (g_count == kRingLen) {
    memmove(g_t, g_t + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_h, g_h + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_p, g_p + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_o, g_o + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_s, g_s + 1, (kRingLen - 1) * sizeof(int8_t));
    memmove(g_bv, g_bv + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_c, g_c + 1, (kRingLen - 1) * sizeof(int8_t));
    g_count--;
  }
  g_t[g_count] = s.t;
  g_h[g_count] = s.h;
  g_p[g_count] = s.p;
  g_o[g_count] = s.out_f;
  g_s[g_count] = s.speed;
  g_bv[g_count] = s.batt_v;
  g_c[g_count] = s.chg;
  g_count++;
  if (stamped)
    g_end_ts = time(nullptr);
}

uint16_t count() { return g_count; }
time_t end_ts() { return g_end_ts; }
const float* temp() { return g_t; }
const float* rh() { return g_h; }
const float* hpa() { return g_p; }
const float* out_f() { return g_o; }
const float* batt_v() { return g_bv; }
const int8_t* speed() { return g_s; }
const int8_t* chg() { return g_c; }

void temp_stats(float* mn, float* mx, float* avg) {
  *mn = *mx = *avg = NAN;
  if (!g_count)
    return;
  float sum = 0;
  for (uint16_t i = 0; i < g_count; i++) {
    const float t = g_t[i];
    if (std::isnan(*mn) || t < *mn)
      *mn = t;
    if (std::isnan(*mx) || t > *mx)
      *mx = t;
    sum += t;
  }
  *avg = sum / g_count;
}

}  // namespace history
