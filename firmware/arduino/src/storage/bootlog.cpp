// See bootlog.h. Append-only, one line per boot, written once the clock is
// trustworthy.
#include "storage/bootlog.h"

#include <Arduino.h>
#include <SD.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "esp_task_wdt.h"
#include "storage/line_reader.h"
#include "system/crashlog.h"
#include "system/eventlog.h"
#include "system/timeutil.h"

namespace bootlog {
namespace {

constexpr const char* kPath = "/boots.csv";
// ~32 bytes a line, so this holds on the order of a thousand boots -- years
// at this device's rate. Rotation drops the OLDEST generation rather than
// growing without bound; the charts only ever ask for the last 60 days.
constexpr uint32_t kRotateBytes = 32 * 1024;
constexpr const char* kOldPath = "/boots.old";

bool g_written = false;

}  // namespace

void tick() {
  if (g_written)
    return;
  // Both preconditions are genuinely required, and neither is permanent: the
  // card mounts lazily and SNTP can take a while after a cold start, so this
  // simply waits rather than giving up.
  if (!sdcard::ok() || !time_synced())
    return;

  // The boot happened `uptime` ago; stamp it there rather than at the moment
  // the clock finally synced, or every marker would sit minutes to the right
  // of the outage it explains.
  const time_t now = time(nullptr);
  const time_t booted = now - static_cast<time_t>(millis() / 1000UL);

  File f = SD.open(kPath, FILE_APPEND);
  if (!f) {
    sdcard::mark_unmounted();  // card yanked; the retry ladder re-detects
    return;
  }
  if (f.size() > kRotateBytes) {
    f.close();
    SD.remove(kOldPath);
    SD.rename(kPath, kOldPath);
    f = SD.open(kPath, FILE_APPEND);
    if (!f) {
      sdcard::mark_unmounted();
      return;
    }
  }
  // The cause is crashlog's word for how the PREVIOUS life ended ("brownout",
  // "panic", "sw_reset"); the version is what THIS life is running. Together
  // they let the chart say "this is where 1.26.0 went on" instead of the
  // cause's ambiguous "sw_reset" -- which covers an OTA and a plain requested
  // restart indistinguishably.
  char line[64];
  const int n =
      snprintf(line, sizeof(line), "%ld,%lu,%s,%s\n", static_cast<long>(booted),
               static_cast<unsigned long>(crashlog::boots()), crashlog::last_death(), FW_VERSION);
  if (n <= 0 || n >= static_cast<int>(sizeof(line))) {
    f.close();
    g_written = true;  // unformattable, and retrying cannot change that
    return;
  }
  const size_t written = f.write(reinterpret_cast<const uint8_t*>(line), n);
  f.close();
  // Same rule sdcard::log_sample follows: a short write means the card is
  // failing or full, and half a line is worse than none -- it would parse as
  // a boot at a garbage epoch. Leave g_written false so the next tick tries
  // again on a remounted card.
  if (written != static_cast<size_t>(n)) {
    sdcard::mark_unmounted();
    return;
  }
  g_written = true;
  eventlog::log("boot", "marked %ld cause=%s", static_cast<long>(booted), crashlog::last_death());
}

uint32_t stream(time_t cutoff, sdcard::LineSink sink, void* ctx) {
  if (!sdcard::ok() || !sink)
    return 0;
  uint32_t sent = 0;
  // Oldest generation first, so the caller always receives chronological
  // order across a rotation boundary.
  for (const char* path : {kOldPath, kPath}) {
    File f = SD.open(path, FILE_READ);
    if (!f)
      continue;
    storage::LineReader<File> rd(f);
    char line[64];
    uint32_t fed = 0;
    while (rd.next(line, sizeof(line))) {
      if ((++fed & 0x3F) == 0)
        esp_task_wdt_reset();
      if (strtol(line, nullptr, 10) < cutoff)
        continue;
      if (!sink(line, ctx)) {
        f.close();
        return sent;
      }
      sent++;
    }
    f.close();
  }
  return sent;
}

}  // namespace bootlog
