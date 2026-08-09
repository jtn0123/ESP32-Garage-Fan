// See eventlog.h. The ring logic itself lives in eventlog_ring.h (natively
// tested); this side owns the device instance, the timestamp prefix, and the
// SD drain.
#include "system/eventlog.h"

#include <Arduino.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>

#include "storage/sdcard.h"
#include "system/eventlog_ring.h"
#include "system/timeutil.h"

namespace eventlog {
namespace {

// 48 lines x 104 bytes = ~5 KB of RAM. WiFi events append from the network
// event task while loop() reads and flushes, hence the spinlock around every
// ring touch (the sections are short memcpys, never I/O).
LineRing<48, 104> g_ring;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

void log(const char* tag, const char* fmt, ...) {
  char msg[80];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  char full[104];
  snprintf(full, sizeof(full), "%ld %lu %s %s", time_synced() ? (long)time(nullptr) : 0L,
           (unsigned long)(millis() / 1000UL), tag, msg);
  portENTER_CRITICAL(&g_mux);
  g_ring.append(full);
  portEXIT_CRITICAL(&g_mux);
  Serial.println(full);
}

void flush_tick() {
  if (!sdcard::ok())
    return;
  // A small batch per tick: the ledger keeps the place, and the cap keeps an
  // event burst from turning one tick into a run of SD writes.
  for (int i = 0; i < 8; i++) {
    char line[104];
    portENTER_CRITICAL(&g_mux);
    const uint16_t pending = g_ring.unflushed();
    if (pending)
      snprintf(line, sizeof(line), "%s", g_ring.line(g_ring.first_unflushed()));
    portEXIT_CRITICAL(&g_mux);
    if (!pending)
      return;
    if (!sdcard::append_event_line(line))
      return;  // write failed and dropped the mount; retry after the remount
    portENTER_CRITICAL(&g_mux);
    g_ring.mark_flushed(1);
    portEXIT_CRITICAL(&g_mux);
  }
}

uint16_t count() {
  portENTER_CRITICAL(&g_mux);
  const uint16_t n = g_ring.n;
  portEXIT_CRITICAL(&g_mux);
  return n;
}

void copy_line(uint16_t i, char* dst, uint32_t cap) {
  portENTER_CRITICAL(&g_mux);
  snprintf(dst, cap, "%s", i < g_ring.n ? g_ring.line(i) : "");
  portEXIT_CRITICAL(&g_mux);
}

uint32_t lost() {
  portENTER_CRITICAL(&g_mux);
  const uint32_t l = g_ring.lost;
  portEXIT_CRITICAL(&g_mux);
  return l;
}

}  // namespace eventlog
