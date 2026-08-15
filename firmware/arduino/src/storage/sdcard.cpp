// See sdcard.h. Mount, log and read paths are verbatim from the pre-split
// firmware; only the state names changed.
#include "storage/sdcard.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "esp_task_wdt.h"
#include "system/crashlog.h"
#include "storage/csv_row.h"
#include "storage/line_reader.h"
#include "storage/purge_logic.h"
#include "system/eventlog.h"

namespace sdcard {
namespace {

bool g_ok = false;
uint8_t g_fails = 0;  // give up retrying after 10; format resets
bool g_quarantined = false;
bool g_net_up = false;  // pulls the first mount attempt forward; see retry_tick

constexpr const char* kEventsPath = "/events.log";
constexpr const char* kEventsOldPath = "/events.old";
constexpr uint32_t kEventsRotateBytes = 512 * 1024;

// This rig's SPI wiring is marginal at speed: a card that ignores 4 MHz often
// answers at 1 MHz or 400 kHz, so every begin() walks this ladder.
static const uint32_t kFreqs[] = {4000000, 1000000, 400000};
constexpr size_t kFreqCount = sizeof(kFreqs) / sizeof(kFreqs[0]);

// Full teardown between attempts, per storage.cpp: a card interrupted
// mid-transaction by a reset stops answering until the bus restarts from
// silence.
static void bus_restart() {
  SD.end();
  SPI.end();
  delay(50);
  SPI.begin();
}

static bool begin_attempt(uint32_t freq, bool format_if_failed) {
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  pinMode(SRAM_CS_PIN, OUTPUT);
  digitalWrite(SRAM_CS_PIN, HIGH);
  pinMode(EPD_CS_PIN, OUTPUT);
  digitalWrite(EPD_CS_PIN, HIGH);
  // max_files=2, not the default 5: with per-file cache enabled each slot
  // embeds a 4 KB sector buffer, and esp_vfs_fat_register allocates all of
  // them plus the FATFS core in ONE contiguous block (~26 KB at 5 slots).
  // After WiFi is up this board's largest free block is ~17 KB, so the
  // default could never mount (NO_MEM, 2026-08-09). We open one file at a
  // time (event log, CSV, tail); 2 slots (~13 KB) is one of margin.
  return format_if_failed ? SD.begin(SD_CS_PIN, SPI, freq, "/sd", 2, true)
                          : SD.begin(SD_CS_PIN, SPI, freq, "/sd", 2);
}

// Reachability outranks persistence. The first time the card ever mounted
// (1.14.6), its ~13 KB filesystem context left the largest free block at
// 1.5 KB -- no socket could allocate, and the board vanished from the network
// while "working". If a mount leaves less than this contiguous, undo it.
constexpr uint32_t kMinLargestAfterMount = 10 * 1024;

// The last mount attempt succeeded but was undone for heap headroom. Heap
// pressure is transient in a way a dead card is not, so retry_tick must not
// charge these against the 10-failure cap -- ten heap rejections would
// otherwise permanently stop retries in exactly the situation that can heal.
bool g_heap_denied = false;

static void mount() {
  g_heap_denied = false;
  for (size_t i = 0; i < kFreqCount; i++) {
    if (i > 0)
      bus_restart();
    if (begin_attempt(kFreqs[i], false)) {
      const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      if (largest < kMinLargestAfterMount) {
        SD.end();
        g_heap_denied = true;
        eventlog::log("sd", "mount undone: leaves largest=%lu (<%lu); network first",
                      (unsigned long)largest, (unsigned long)kMinLargestAfterMount);
        return;
      }
      g_ok = true;
      eventlog::log("sd", "mounted at %lu Hz: %lu MB (largest=%lu)", (unsigned long)kFreqs[i],
                    (unsigned long)(SD.totalBytes() / 1048576), (unsigned long)largest);
      return;
    }
  }
  // On the flight-recorder tape, not just Serial: silent mount failures made
  // "why is the card not logging" undiagnosable from the network. Heap
  // numbers included because the 2026-08-09 failure was esp_vfs_fat_register
  // returning NO_MEM -- the card was never the problem, the allocator was.
  eventlog::log("sd", "mount failed at all %u freqs (heap=%lu largest=%lu)", (unsigned)kFreqCount,
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

}  // namespace

void mount_guarded() {
  crashlog::rtc_sd_sentinel = crashlog::kSdSentinelMagic;
  CRUMB("sd_mount");
  mount();
  CRUMB_CLEAR();
  crashlog::rtc_sd_sentinel = 0;
}

uint32_t free_mb() {
  if (!g_ok)
    return 0;
  const uint64_t total = SD.totalBytes();
  const uint64_t used = SD.usedBytes();
  return used >= total ? 0 : static_cast<uint32_t>((total - used) / 1048576);
}

bool format() {
  Serial.println("[SD] format requested");
  esp_task_wdt_delete(NULL);  // a big-card format legitimately takes minutes
  g_quarantined = false;      // manual override un-quarantines
  crashlog::rtc_sd_sentinel = crashlog::kSdSentinelMagic;
  CRUMB("sd_format");
  // The same frequency ladder as mount(): a format that only ever tried
  // 4 MHz returned 500 on a healthy 32 GB card (2026-08-09) that the ladder
  // handshakes fine -- the failure was the bus speed, not the card.
  bool ok = false;
  for (size_t i = 0; i < kFreqCount && !ok; i++) {
    bus_restart();
    ok = begin_attempt(kFreqs[i], true);
  }
  crashlog::rtc_sd_sentinel = 0;
  CRUMB_CLEAR();
  esp_task_wdt_add(NULL);
  // Same headroom rule mount() enforces, for the same reason: a successful
  // mount whose filesystem context eats the last contiguous block takes the
  // board off the network while it looks healthy, and this path is reachable
  // from a button in the console. Without this check format() was the one way
  // to reach that state and then be unable to call back in to undo it.
  if (ok) {
    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largest < kMinLargestAfterMount) {
      SD.end();
      ok = false;
      g_heap_denied = true;
      eventlog::log("sd", "format mounted but undone: largest=%lu (<%lu); network first",
                    (unsigned long)largest, (unsigned long)kMinLargestAfterMount);
    }
  }
  g_ok = ok;
  g_fails = 0;
  return ok;
}

bool append_event_line(const char* line) {
  if (!g_ok)
    return false;
  CRUMB("sd_evt_w");
  File f = SD.open(kEventsPath, FILE_APPEND);
  if (!f) {
    CRUMB_CLEAR();
    g_ok = false;  // card yanked; re-detect via the retry ladder
    return false;
  }
  if (f.size() > kEventsRotateBytes) {
    // One rotation generation: half a MB of events is weeks of history, and
    // the pulled-card reader gets at most two bounded files to think about.
    f.close();
    SD.remove(kEventsOldPath);
    SD.rename(kEventsPath, kEventsOldPath);
    f = SD.open(kEventsPath, FILE_APPEND);
    if (!f) {
      CRUMB_CLEAR();
      g_ok = false;
      return false;
    }
  }
  const size_t len = strlen(line);
  size_t written = f.write(reinterpret_cast<const uint8_t*>(line), len);
  written += f.write('\n');
  f.close();
  CRUMB_CLEAR();
  if (written != len + 1) {
    g_ok = false;  // failing writes; remount rather than lose lines silently
    return false;
  }
  return true;
}

uint32_t tail_events(char* buf, uint32_t cap) {
  if (!buf || cap < 2)
    return 0;  // nothing writable; do not touch the buffer at all
  buf[0] = '\0';
  if (!g_ok)
    return 0;
  CRUMB("sd_evt_r");
  File f = SD.open(kEventsPath, FILE_READ);
  if (!f) {
    CRUMB_CLEAR();
    return 0;
  }
  const uint32_t size = f.size();
  const uint32_t want = cap - 1;
  if (size > want)
    f.seek(size - want);
  const int got = f.read(reinterpret_cast<uint8_t*>(buf), want);
  f.close();
  CRUMB_CLEAR();
  const uint32_t n = got > 0 ? static_cast<uint32_t>(got) : 0;
  buf[n] = '\0';
  return n;
}

bool ok() { return g_ok; }
bool quarantined() { return g_quarantined; }
void set_quarantined(bool q) { g_quarantined = q; }
void mark_unmounted() { g_ok = false; }
uint32_t total_mb() { return g_ok ? static_cast<uint32_t>(SD.totalBytes() / 1048576) : 0; }
uint32_t used_mb() { return g_ok ? static_cast<uint32_t>(SD.usedBytes() / 1048576) : 0; }

void retry_tick() {
  // First mount attempt the moment WiFi associates, then every 10th call
  // (~5 min at the auto-tick cadence), 10-fail cap, never while quarantined.
  // Timing is the whole game: esp_vfs_fat_register needs ONE contiguous
  // ~13 KB block, and every second of web/MQTT traffic fragments the heap
  // (largest free block measured at 76 s after boot: 17 KB on a quiet boot,
  // 7.7 KB on a busy one -- both boots had 20+ KB "free"). Right after the
  // radio settles is the freshest heap this board ever has. The 60 s clause
  // is the WiFi-never-came fallback; before-WiFi mounting is off the table
  // (1.14.3: the radio failed to come up at all).
  static bool tried = false;
  static uint8_t backoff = 0;
  if (g_ok || g_quarantined || g_fails >= 10)
    return;
  const bool due = tried ? (++backoff >= 10) : (g_net_up || millis() > 60000);
  if (!due)
    return;
  tried = true;
  backoff = 0;
  mount_guarded();
  // A heap-denied attempt proved the CARD is fine (it mounted); only the
  // moment was wrong. Charging it against the cap would end all retries
  // after ten tight-heap ticks -- the one failure mode that heals itself.
  if (g_ok)
    g_fails = 0;
  else if (!g_heap_denied)
    g_fails++;
}

void on_network_up() {
  g_net_up = true;
  retry_tick();
}

}  // namespace sdcard
