// See history.h.
#include "storage/history.h"

#include <esp_heap_caps.h>

#include <new>

#include "config.h"
#include "system/reboot.h"

namespace history {
namespace {

// In PSRAM, not .bss: the watts + gas columns (1.14.47) grew the ring past
// what the S2's DRAM segment had left (linker: dram0_0_seg overflowed by
// 2.9 KB). Placement-new into a PSRAM block; every access is a column scan
// for a once-per-request JSON writer, which PSRAM serves fine. Falls back to
// internal heap on a board without PSRAM.
Ring<kRingLen>* g_ring = nullptr;
time_t g_end_ts = 0;

Ring<kRingLen>& ring() {
  if (!g_ring) {
    void* m = heap_caps_calloc(1, sizeof(Ring<kRingLen>), MALLOC_CAP_SPIRAM);
    if (!m)
      m = heap_caps_calloc(1, sizeof(Ring<kRingLen>), MALLOC_CAP_8BIT);
    if (!m) {
      // Placement-new on null is UB and every accessor would then scribble
      // through it. This allocates once, in early setup, when the heap is at
      // its emptiest -- if BOTH pools refuse ~10 KB here, the boot is already
      // doomed, and a reboot with the reason on the tape beats corruption.
      sysreboot::restart("no memory for the history ring");
    }
    g_ring = new (m) Ring<kRingLen>();
  }
  return *g_ring;
}

}  // namespace

void append(const Sample& s, bool stamped) {
  ring().append(s);
  if (stamped)
    g_end_ts = time(nullptr);
}

uint16_t count() { return ring().n; }
time_t end_ts() { return g_end_ts; }
const float* temp() { return ring().t; }
const float* rh() { return ring().h; }
const float* hpa() { return ring().p; }
const float* out_f() { return ring().o; }
const float* batt_v() { return ring().bv; }
const float* watts() { return ring().w; }
const float* bme_t() { return ring().bt; }
const float* bme_rh() { return ring().bh; }
const int32_t* voc_raw() { return ring().vr; }
const int32_t* nox_raw() { return ring().nr; }
const int16_t* voc() { return ring().vi; }
const int16_t* nox() { return ring().ni; }
const int8_t* speed() { return ring().s; }
const int8_t* chg() { return ring().c; }

void temp_stats(float* mn, float* mx, float* avg) { ring().temp_stats(mn, mx, avg); }

}  // namespace history
