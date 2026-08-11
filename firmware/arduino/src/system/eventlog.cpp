// See eventlog.h. The ring logic itself lives in eventlog_ring.h (natively
// tested); this side owns the device instance, the timestamp prefix, and the
// SD drain.
#include "system/eventlog.h"

#include <Arduino.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_log.h"
#include "storage/sdcard.h"
#include "system/eventlog_ring.h"
#include "system/log_filter.h"
#include "system/timeutil.h"

namespace eventlog {
namespace {

// 24 lines x 104 bytes = ~2.5 KB of RAM -- an hour-plus of normal chatter,
// and the SD file carries the long history. Halved from 48 on 2026-08-09:
// this board's heap is tight enough that esp_vfs_fat_register was failing
// with NO_MEM, and the ring was one of the few honest places to give back.
// WiFi events append from the network event task while loop() reads and
// flushes, hence the spinlock around every ring touch (the sections are
// short memcpys, never I/O).
LineRing<24, 104> g_ring;
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
  // Timed because this is the loop's other blocking SD path and it was NOT
  // instrumented when sample_climate was: most ticks flush nothing and cost
  // nothing, so a stall here hides between samples and looks like a random
  // MQTT dropout. A block past the 15 s keepalive kills the broker session
  // (2026-08-09).
  const uint32_t t0 = millis();
  int wrote = 0;
  // A small batch per tick: the ledger keeps the place, and the cap keeps an
  // event burst from turning one tick into a run of SD writes.
  for (int i = 0; i < 8; i++) {
    char line[104];
    uint32_t seq = 0;
    portENTER_CRITICAL(&g_mux);
    const uint16_t pending = g_ring.unflushed();
    if (pending) {
      // The ledger IS the sequence: `flushed` is the lifetime index of the
      // first unflushed line, i.e. of the line copied here.
      seq = g_ring.flushed;
      snprintf(line, sizeof(line), "%s", g_ring.line(g_ring.first_unflushed()));
    }
    portEXIT_CRITICAL(&g_mux);
    if (!pending)
      break;
    if (!sdcard::append_event_line(line))
      break;  // write failed and dropped the mount; retry after the remount
    wrote++;
    portENTER_CRITICAL(&g_mux);
    // Acknowledge only if the ledger still points at the line just written.
    // An event burst during the SD write can wrap the ring past it (append's
    // guard advances `flushed` and counts it lost); blindly marking one
    // flushed here would then skip a line that never reached the file.
    if (g_ring.flushed == seq)
      g_ring.mark_flushed(1);
    portEXIT_CRITICAL(&g_mux);
  }
  // Safe to log here: the flush loop is finished, so this line simply lands in
  // the ring and rides out on the next tick rather than re-entering the batch.
  const uint32_t dt = millis() - t0;
  if (dt > 2000)
    log("slow", "sdflush %lums lines=%d", (unsigned long)dt, wrote);
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

namespace {

// esp_log vprintf hook: framework warnings become tape lines. 20 lines per
// 10 s window, then dropped -- the SD driver's dying words fit easily; a
// boot-time log storm must not evict the events that matter.
// The noise decision itself lives in log_filter.h so the host can test it --
// a mis-tuned filter is otherwise only discovered when a real fault turns out
// to be missing from the tape.
int esp_vlog_to_ring(const char* fmt, va_list ap) {
  static uint32_t win_start_ms = 0;
  static uint8_t in_window = 0;
  static log_filter::NoiseGate gate;
  char buf[160];
  const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  for (int i = 0; buf[i]; i++)
    if (buf[i] == '\n' || buf[i] == '\r')
      buf[i] = ' ';

  const bool noisy = log_filter::is_i2c_noise(buf);
  const auto v = gate.feed(noisy, millis());
  if (noisy) {
    if (v.summary)
      log("i2c", "%lu benign bus errors suppressed since last report", (unsigned long)v.count);
    else if (v.emit)
      log("esp", "%s", buf);
    return n;
  }

  if (millis() - win_start_ms > 10000) {
    win_start_ms = millis();
    in_window = 0;
  }
  if (in_window < 20) {
    in_window++;
    log("esp", "%s", buf);
  }
  return n;
}

}  // namespace

void capture_esp_logs() {
  esp_log_level_set("*", ESP_LOG_WARN);
  esp_log_set_vprintf(&esp_vlog_to_ring);
}

}  // namespace eventlog
