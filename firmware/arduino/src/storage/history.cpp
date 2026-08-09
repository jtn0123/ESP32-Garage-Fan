// See history.h.
#include "storage/history.h"

#include "config.h"

namespace history {
namespace {

Ring<kRingLen> g_ring;
time_t g_end_ts = 0;

}  // namespace

void append(const Sample& s, bool stamped) {
  g_ring.append(s);
  if (stamped)
    g_end_ts = time(nullptr);
}

uint16_t count() { return g_ring.n; }
time_t end_ts() { return g_end_ts; }
const float* temp() { return g_ring.t; }
const float* rh() { return g_ring.h; }
const float* hpa() { return g_ring.p; }
const float* out_f() { return g_ring.o; }
const float* batt_v() { return g_ring.bv; }
const int8_t* speed() { return g_ring.s; }
const int8_t* chg() { return g_ring.c; }

void temp_stats(float* mn, float* mx, float* avg) { g_ring.temp_stats(mn, mx, avg); }

}  // namespace history
