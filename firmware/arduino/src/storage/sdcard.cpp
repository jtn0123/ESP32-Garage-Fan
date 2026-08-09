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

static void mount() {
  for (size_t i = 0; i < kFreqCount; i++) {
    if (i > 0)
      bus_restart();
    if (begin_attempt(kFreqs[i], false)) {
      g_ok = true;
      eventlog::log("sd", "mounted at %lu Hz: %lu MB", (unsigned long)kFreqs[i],
                    (unsigned long)(SD.totalBytes() / 1048576));
      return;
    }
  }
  // On the flight-recorder tape, not just Serial: silent mount failures made
  // "why is the card not logging" undiagnosable from the network. Heap
  // numbers included because the 2026-08-09 failure was esp_vfs_fat_register
  // returning NO_MEM -- the card was never the problem, the allocator was.
  eventlog::log("sd", "mount failed at all %u freqs (heap=%lu largest=%lu)",
                (unsigned)kFreqCount, (unsigned long)ESP.getFreeHeap(),
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

void log_sample(time_t now, float t, float h, float p, float out_f, int speed) {
  if (!g_ok)
    return;
  struct tm tm_now;
  gmtime_r(&now, &tm_now);
  char path[36];  // worst-case int expansion of the two %d fields, not 4+2
  snprintf(path, sizeof(path), "/climate-%04d%02d.csv", tm_now.tm_year + 1900, tm_now.tm_mon + 1);
  CRUMB("sd_write");
  File f = SD.open(path, FILE_APPEND);
  if (!f) {
    CRUMB_CLEAR();
    g_ok = false;  // card yanked; re-detect on reboot
    return;
  }
  char line[96];
  int n = snprintf(line, sizeof(line), "%ld,%.2f,%.1f,%.1f,%.2f,%d\n", (long)now, t, h, p,
                   isnan(out_f) ? -999.0f : out_f, speed);
  if (n < 0 || n >= static_cast<int>(sizeof(line))) {
    f.close();  // a truncated row would corrupt the CSV for every later reader
    CRUMB_CLEAR();
    return;
  }
  const size_t written = f.write((const uint8_t*)line, n);
  f.close();
  if (written != static_cast<size_t>(n))
    g_ok = false;  // card is failing writes; re-mount rather than lose rows silently
  CRUMB_CLEAR();
}

// Stream the month files covering [cutoff, now], decimating by stride into
// the caller's arrays. Two passes: count, then collect every (count/max)th.
uint16_t read_range(time_t cutoff, float* t, float* h, float* p, uint16_t max_pts) {
  CRUMB("sd_read");
  time_t now = time(nullptr);
  char paths[2][36];
  int npaths = 0;
  for (time_t at = cutoff; npaths < 2; at = now) {
    struct tm tmv;
    gmtime_r(&at, &tmv);
    char path[36];
    snprintf(path, sizeof(path), "/climate-%04d%02d.csv", tmv.tm_year + 1900, tmv.tm_mon + 1);
    if (npaths == 0 || strcmp(path, paths[0]) != 0) {
      snprintf(paths[npaths], sizeof(paths[npaths]), "%s", path);
      npaths++;
    }
    if (at == now)
      break;
  }
  uint32_t rows = 0;
  for (int pass = 0; pass < 2; pass++) {
    const uint32_t stride = pass ? (rows > max_pts ? rows / max_pts + 1 : 1) : 1;
    uint32_t seen = 0;
    uint16_t kept = 0;
    for (int i = 0; i < npaths; i++) {
      File f = SD.open(paths[i], FILE_READ);
      if (!f)
        continue;
      // Line-buffered scan; lines are short and epoch-prefixed.
      char line[96];
      size_t ll = 0;
      uint32_t fed = 0;
      while (f.available()) {
        // Two passes over up to two month files is normally ~3 s, but a
        // corrupt oversized file must not ride into the 60 s task watchdog.
        if ((++fed & 0x3FF) == 0)
          esp_task_wdt_reset();
        const char c = static_cast<char>(f.read());
        if (c != '\n') {
          if (ll < sizeof(line) - 1)
            line[ll++] = c;
          continue;
        }
        line[ll] = '\0';
        ll = 0;
        const long epoch = strtol(line, nullptr, 10);
        if (epoch < cutoff)
          continue;
        if (pass == 0) {
          rows++;
          continue;
        }
        if (seen++ % stride)
          continue;
        if (kept >= max_pts)
          break;
        float tv, hv, pv;
        if (sscanf(line, "%*d,%f,%f,%f", &tv, &hv, &pv) == 3) {
          t[kept] = tv;
          h[kept] = hv;
          p[kept] = pv;
          kept++;
        }
      }
      f.close();
    }
    if (pass == 1) {
      CRUMB_CLEAR();
      return kept;
    }
    if (rows == 0) {
      CRUMB_CLEAR();
      return 0;
    }
  }
  CRUMB_CLEAR();
  return 0;
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
  g_fails = g_ok ? 0 : g_fails + 1;
}

void on_network_up() {
  g_net_up = true;
  retry_tick();
}

}  // namespace sdcard
