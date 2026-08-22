// The CSV row store: one appender and two readers over the month files.
// Split from sdcard.cpp when it passed 700 lines. Mount state stays in
// sdcard.cpp; this file reaches it only through ok()/mark_unmounted(), the
// same surface every other module uses.
#include <Arduino.h>
#include <SD.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_task_wdt.h"
#include "storage/csv_row.h"
#include "storage/line_reader.h"
#include "storage/sdcard.h"
#include "system/crashlog.h"

namespace sdcard {

void log_sample(time_t now, float t, float h, float p, float out_f, int speed, float batt_v,
                int chg, float watts, int32_t voc_raw, int32_t nox_raw, int voc, int nox, int flips,
                float w_min, float w_max) {
  if (!ok())
    return;
  struct tm tm_now;
  gmtime_r(&now, &tm_now);
  char path[36];  // worst-case int expansion of the two %d fields, not 4+2
  snprintf(path, sizeof(path), "/climate-%04d%02d.csv", tm_now.tm_year + 1900, tm_now.tm_mon + 1);
  CRUMB("sd_write");
  File f = SD.open(path, FILE_APPEND);
  if (!f) {
    CRUMB_CLEAR();
    mark_unmounted();  // card yanked; re-detect on reboot
    return;
  }
  char line[160];
  // batt_v/chg appended 1.14.23; watts + the four SGP41 columns appended
  // 1.14.47 (plug meter and the air chain). bme_t/bme_rh columns existed
  // 1.14.48-1.15.1 (the dual-thermometer comparison that CONVICTED the
  // BME280's thermometer of self-heating); rows went back to 13 fields when
  // it became barometer-only, 1.21.0 appended the 14th (the plug meter's
  // run/stop flip count for the bucket -- the cycling profile,
  // net/plug_cycle.h) and 1.22.0 appended the 15th and 16th: the bucket's
  // draw range, because the single `watts` snapshot is what let a fan
  // cycling every minute chart as jitter. read_range parses every historical
  // width -- 14 fields is flips, 15 is the old BME pair, 16 is this one (see
  // csv_row.h; the width decides, and the two eras never share a card).
  // -999 is the "no reading" sentinel for float columns, -1 for gas and flips.
  int n = snprintf(line, sizeof(line),
                   "%ld,%.2f,%.1f,%.1f,%.2f,%d,%.2f,%d,%.1f,%ld,%ld,%d,%d,%d,%.1f,%.1f\n",
                   (long)now, t, h, isnan(p) ? -999.0f : p, isnan(out_f) ? -999.0f : out_f, speed,
                   isnan(batt_v) ? -999.0f : batt_v, chg, isnan(watts) ? -999.0f : watts,
                   (long)voc_raw, (long)nox_raw, voc, nox, flips, isnan(w_min) ? -999.0f : w_min,
                   isnan(w_max) ? -999.0f : w_max);
  if (n < 0 || n >= static_cast<int>(sizeof(line))) {
    f.close();  // a truncated row would corrupt the CSV for every later reader
    CRUMB_CLEAR();
    return;
  }
  const size_t written = f.write((const uint8_t*)line, n);
  f.close();
  if (written != static_cast<size_t>(n))
    mark_unmounted();  // card is failing writes; re-mount rather than lose rows silently
  CRUMB_CLEAR();
}

// Stream the month files covering [cutoff, now], decimating by stride into
// the caller's arrays. Two passes: count, then collect every (count/max)th.
// The month files spanning [cutoff, now]. At most FOUR: a 60-day window can
// touch four calendar months (Jan 31 to Apr 1 is exactly 60 days in a
// non-leap year and covers Jan, Feb, Mar, Apr). The two-slot version dropped
// the OLDEST month; a three-slot version would have dropped the NEWEST on
// that boundary -- either way a chart quietly missing a month, the class of
// confident falsehood handle_history's validation exists to prevent.
static int month_paths(time_t cutoff, char paths[4][36]) {
  const time_t now = time(nullptr);
  int npaths = 0;
  for (time_t at = cutoff; npaths < 4;) {
    struct tm tmv;
    gmtime_r(&at, &tmv);
    char path[36];
    snprintf(path, sizeof(path), "/climate-%04d%02d.csv", tmv.tm_year + 1900, tmv.tm_mon + 1);
    if (npaths == 0 || strcmp(path, paths[npaths - 1]) != 0) {
      snprintf(paths[npaths], sizeof(paths[npaths]), "%s", path);
      npaths++;
    }
    if (at >= now)
      break;
    // 28-day hops visit every month in order without needing timegm (absent
    // on this libc; mktime would apply the local TZ): no month is shorter,
    // so a hop cannot skip one, and the adjacent-name dedupe above absorbs
    // landing in the same month twice.
    at += 28L * 86400;
    if (at > now)
      at = now;
  }
  return npaths;
}

uint32_t stream_range(time_t cutoff, LineSink sink, void* ctx) {
  if (!ok() || !sink)
    return 0;
  CRUMB("sd_stream");
  char paths[4][36];
  const int npaths = month_paths(cutoff, paths);
  uint32_t sent = 0;
  for (int i = 0; i < npaths; i++) {
    File f = SD.open(paths[i], FILE_READ);
    if (!f)
      continue;
    storage::LineReader<File> rd(f);
    char line[96];
    uint32_t fed = 0;
    while (rd.next(line, sizeof(line))) {
      if ((++fed & 0x3F) == 0)
        esp_task_wdt_reset();
      if (strtol(line, nullptr, 10) < cutoff)
        continue;
      if (!sink(line, ctx)) {
        f.close();
        CRUMB_CLEAR();
        return sent;
      }
      sent++;
    }
    f.close();
  }
  CRUMB_CLEAR();
  return sent;
}

uint16_t read_range(time_t cutoff, const Samples& out, uint16_t max_pts) {
  CRUMB("sd_read");
  char paths[4][36];
  const int npaths = month_paths(cutoff, paths);
  uint32_t rows = 0;
  for (int pass = 0; pass < 2; pass++) {
    const uint32_t stride = pass ? (rows > max_pts ? rows / max_pts + 1 : 1) : 1;
    uint32_t seen = 0;
    uint16_t kept = 0;
    for (int i = 0; i < npaths; i++) {
      File f = SD.open(paths[i], FILE_READ);
      if (!f)
        continue;
      // Block-buffered scan; lines are short and epoch-prefixed.
      storage::LineReader<File> rd(f);
      char line[96];
      uint32_t fed = 0;
      while (rd.next(line, sizeof(line))) {
        // Two passes over up to two month files, but a corrupt oversized file
        // must not ride into the 60 s task watchdog.
        if ((++fed & 0x3F) == 0)
          esp_task_wdt_reset();
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
        // Both widths and both sentinels live in storage::parse_csv_row, which
        // the host tests exercise directly (native_csv_row).
        const storage::CsvRow row = storage::parse_csv_row(line);
        if (!row.valid)
          continue;
        out.ts[kept] = static_cast<time_t>(row.epoch);
        out.temp_c[kept] = row.temp_c;
        out.rh[kept] = row.rh;
        out.hpa[kept] = row.hpa;
        out.out_f[kept] = row.out_f;
        out.spd[kept] = row.spd;
        out.batt_v[kept] = row.batt_v;
        out.chg[kept] = row.chg;
        out.watts[kept] = row.watts;
        out.voc_raw[kept] = row.voc_raw;
        out.nox_raw[kept] = row.nox_raw;
        out.voc[kept] = row.voc;
        out.nox[kept] = row.nox;
        out.flips[kept] = row.flips;
        out.w_min[kept] = row.w_min;
        out.w_max[kept] = row.w_max;
        kept++;
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

}  // namespace sdcard
