// microSD storage implementation
// Copyright 2024 Justin

#include "storage.h"

#include <math.h>
#include <time.h>
#include <cstdarg>
#include <cstdio>

// logging.h is outside the feature guard because it declares log_mirror(), whose
// definition at the bottom of this file has to exist in both build variants.
#include "logging.h"

// SD.h/SPI.h stay behind the feature guard so a FEATURE_STORAGE=0 build does
// not pull the SD library into the link at all.
#if FEATURE_STORAGE

#include <FFat.h>
#include <SD.h>
#include <SPI.h>
#include <climits>
#include "safe_strings.h"
#include "runtime_config.h"

// At file scope rather than in the anonymous namespace below: cpplint reads the
// wrapped continuation line as an indented namespace body.
static constexpr const char* kHistoryHeader =
    "iso_time,uptime_s,temp_c,rh_pct,press_hpa,batt_v,batt_pct,rssi_dbm,out_temp_c,out_rh_pct\n";

namespace {

constexpr const char* kConfigPath = "/config/device.json";
constexpr const char* kDataDir = "/data";
constexpr const char* kLogDir = "/logs";
constexpr const char* kFirmwareDir = "/firmware";
constexpr const char* kLogIndexPath = "/logs/index.txt";

// The config file is a handful of keys; refuse anything suspiciously large
// rather than trying to allocate it.
constexpr size_t kMaxConfigBytes = 4096;

StorageInfo g_info;
bool g_mounted = false;
uint8_t g_log_index = 0;
bool g_log_index_loaded = false;

// Which filesystem backs the storage paths. The sd_ prefix on this module's API
// is historical: when no card responds, the same layout lives on the board's
// internal 960 KB `ffat` partition instead, and every consumer is none the
// wiser. SD wins when both are available because it is removable — a card can
// be edited on a computer, which internal flash never can.
enum class Backend : uint8_t { kNone, kSdCard, kInternal };
Backend g_backend = Backend::kNone;
fs::FS* g_fs = nullptr;

// Keep this much of the internal partition free. One day's CSV is ~16 KB at a
// 5-minute cadence, so this floor forces the oldest day out well before an
// append can fail mid-write.
constexpr uint64_t kInternalFreeFloorBytes = 64 * 1024;

void set_error(const char* msg) { safe_strcpy(g_info.last_error, msg); }

// A year below 2020 means NTP has not landed yet and the RTC is at its epoch
// default; timestamps derived from it would be worse than useless.
bool epoch_is_plausible(time_t epoch) { return epoch > SD_MIN_PLAUSIBLE_EPOCH; }

void ensure_dir(const char* path) {
  if (!g_fs->exists(path)) {
    g_fs->mkdir(path);
  }
}

// Build "/data/YYYY-MM-DD.csv" for the given time, or the undated fallback.
void history_path_for(time_t epoch, char* out, size_t out_size) {
  if (!epoch_is_plausible(epoch)) {
    snprintf(out, out_size, "%s/nodate.csv", kDataDir);
    return;
  }
  struct tm tm_local;
  localtime_r(&epoch, &tm_local);
  snprintf(out, out_size, "%s/%04d-%02d-%02d.csv", kDataDir, tm_local.tm_year + 1900,
           tm_local.tm_mon + 1, tm_local.tm_mday);
}

void log_path_for(uint8_t index, char* out, size_t out_size) {
  snprintf(out, out_size, "%s/log%u.txt", kLogDir, static_cast<unsigned>(index));
}

// The active log file survives reboots via /logs/index.txt so a restart does not
// clobber the log that was just written.
void load_log_index() {
  if (g_log_index_loaded)
    return;
  g_log_index_loaded = true;

  File f = g_fs->open(kLogIndexPath, FILE_READ);
  if (!f)
    return;
  char buf[8] = {0};
  size_t n = f.readBytes(buf, sizeof(buf) - 1);
  f.close();
  if (n == 0)
    return;
  long parsed = strtol(buf, nullptr, 10);
  if (parsed >= 0 && parsed < SD_LOG_FILE_COUNT) {
    g_log_index = static_cast<uint8_t>(parsed);
  }
}

void save_log_index() {
  File f = g_fs->open(kLogIndexPath, FILE_WRITE);
  if (!f)
    return;
  f.printf("%u\n", static_cast<unsigned>(g_log_index));
  f.close();
}

// Parse "YYYY-MM-DD.csv" into a day number (days since epoch) for retention
// comparisons. Returns false for anything that is not a dated history file,
// which keeps nodate.csv and stray files from being pruned.
bool history_day_from_name(const char* name, long* out_days) {
  // sscanf's return value counts conversions, not literal matches, so a
  // trailing ".csv" in the format would still "succeed" on nodate.txt.
  // Check the shape explicitly instead.
  constexpr size_t kNameLen = sizeof("YYYY-MM-DD.csv") - 1;
  if (strlen(name) != kNameLen)
    return false;
  if (strcmp(name + kNameLen - 4, ".csv") != 0)
    return false;

  int y = 0, m = 0, d = 0;
  if (sscanf(name, "%4d-%2d-%2d", &y, &m, &d) != 3)
    return false;
  if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31)
    return false;

  struct tm tm_file = {};
  tm_file.tm_year = y - 1900;
  tm_file.tm_mon = m - 1;
  tm_file.tm_mday = d;
  tm_file.tm_hour = 12;  // midday avoids DST edges shifting the day
  time_t t = mktime(&tm_file);
  if (t == static_cast<time_t>(-1))
    return false;

  *out_days = static_cast<long>(t / 86400);
  return true;
}

// Drop the oldest dated history file. Used only on the internal backend, where
// 960 KB fills in about two months and someone has to lose.
bool remove_oldest_history() {
  File dir = g_fs->open(kDataDir);
  if (!dir)
    return false;

  char oldest[48] = {0};
  long oldest_day = LONG_MAX;
  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      const char* full = entry.name();
      const char* base = strrchr(full, '/');
      base = base ? base + 1 : full;
      long day = 0;
      if (history_day_from_name(base, &day) && day < oldest_day) {
        oldest_day = day;
        snprintf(oldest, sizeof(oldest), "%s/%s", kDataDir, base);
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  if (!oldest[0])
    return false;
  LOG_WARN("Storage: internal flash low - dropping %s", oldest);
  return g_fs->remove(oldest);
}

}  // namespace

bool storage_begin() {
  if (g_mounted)
    return true;

  g_info = StorageInfo();

  // Park the Wing's SRAM chip select before anything reads the bus. That chip is
  // the only other device on MISO, and neither GxEPD2 nor this firmware ever
  // selects it — but an input-floating CS is not the same as a deasserted one,
  // and a half-selected SRAM answering over the card is exactly the sort of
  // intermittent mount failure that looks like a bad card.
  pinMode(SRAM_CS_PIN, OUTPUT);
  digitalWrite(SRAM_CS_PIN, HIGH);

  // The SD driver only ever calls beginTransaction(); it never starts the bus
  // itself. With a display build GxEPD2 has already done so, but the headless
  // build has no other SPI user, so do it here. SPIClass::begin() early-returns
  // when the bus is already up, which makes this safe in both cases — and it is
  // what lets the card share the bus with the panel (panel CS D9, card CS D5).
  SPI.begin();

  // Walk the bus clock down rather than giving up on the first refusal. A card
  // that will not enumerate at the configured speed often will at the 400 kHz
  // rate the SD spec mandates for initialisation, and a slow mount beats no card.
  // Each attempt reports separately so a log from the field distinguishes "no
  // card in the slot" from "card present, bus will not negotiate".
  static const uint32_t kMountFreqs[] = {SD_SPI_FREQ_HZ, 4000000, 1000000, 400000};
  bool mounted = false;
  for (size_t i = 0; i < sizeof(kMountFreqs) / sizeof(kMountFreqs[0]); i++) {
    if (i > 0) {
      // A card interrupted mid-transaction (a reset landing between command and
      // response -- routine during OTA/flash sessions) can stop answering
      // entirely: every frequency then reports cardType=0 even though the card
      // is fine. The card keeps 3.3 V across ESP resets, so nothing else clears
      // its state machine. Tear the whole bus down and let SD.begin()'s init
      // sequence (74+ clocks with CS high, then CMD0) start from silence.
      SD.end();
      SPI.end();
      delay(50);
      SPI.begin();
    }
    // Re-park the SRAM select each pass: a failed SD.begin() can leave the bus
    // pins reconfigured.
    pinMode(SRAM_CS_PIN, OUTPUT);
    digitalWrite(SRAM_CS_PIN, HIGH);

    if (SD.begin(SD_CS_PIN, SPI, kMountFreqs[i])) {
      LOG_INFO("SD: mounted at %lu Hz (attempt %u)", static_cast<unsigned long>(kMountFreqs[i]),
               static_cast<unsigned>(i + 1));
      mounted = true;
      break;
    }
    // INFO, not WARN: a cardless node falls back to internal flash by design,
    // and four warnings per boot would cry wolf on a configuration that is fine.
    LOG_INFO("SD: begin failed at %lu Hz (CS=%d, cardType=%u)",
             static_cast<unsigned long>(kMountFreqs[i]), SD_CS_PIN,
             static_cast<unsigned>(SD.cardType()));
    SD.end();
    delay(10);
  }

  if (mounted && SD.cardType() == CARD_NONE) {
    SD.end();
    mounted = false;
    LOG_WARN("SD: bus answered but no card detected");
  }

  if (mounted) {
    g_backend = Backend::kSdCard;
    g_fs = &SD;
    switch (SD.cardType()) {
      case CARD_MMC:
        g_info.type = "MMC";
        break;
      case CARD_SD:
        g_info.type = "SDSC";
        break;
      case CARD_SDHC:
        g_info.type = "SDHC";
        break;
      default:
        g_info.type = "unknown";
        break;
    }
    g_info.total_bytes = SD.totalBytes();
    g_info.used_bytes = SD.usedBytes();
  } else {
    // No card responded — fall back to the internal `ffat` partition (960 KB in
    // the stock Adafruit table, unused until now). Same layout, same paths; the
    // one thing it cannot do is be pulled out and edited on a computer.
    // format-on-fail is deliberate: the partition ships from the factory
    // unformatted, and the first boot is exactly when formatting it is cheap.
    if (!FFat.begin(true)) {
      set_error("no card, FFat mount failed");
      LOG_WARN("Storage: no card and internal FFat failed - running without storage");
      return false;
    }
    g_backend = Backend::kInternal;
    g_fs = &FFat;
    g_info.type = "FFat";
    g_info.total_bytes = FFat.totalBytes();
    g_info.used_bytes = FFat.usedBytes();
    LOG_INFO("Storage: no card - using internal flash");
  }

  g_info.mounted = true;
  g_mounted = true;

  ensure_dir(kDataDir);
  ensure_dir(kLogDir);
  ensure_dir(kFirmwareDir);

  LOG_INFO("Storage: mounted %s, %llu KB total, %llu KB used", g_info.type,
           g_info.total_bytes / 1024ULL, g_info.used_bytes / 1024ULL);
  return true;
}

void storage_end() {
  if (!g_mounted)
    return;
  if (g_backend == Backend::kSdCard) {
    SD.end();
  } else if (g_backend == Backend::kInternal) {
    FFat.end();
  }
  g_backend = Backend::kNone;
  g_fs = nullptr;
  g_mounted = false;
  g_info.mounted = false;
}

bool storage_is_mounted() { return g_mounted; }
fs::FS* storage_fs() { return g_mounted ? g_fs : nullptr; }

const StorageInfo& storage_get_info() { return g_info; }

bool storage_load_config() {
  if (!g_mounted)
    return false;

  if (!g_fs->exists(kConfigPath)) {
    LOG_INFO("SD: no %s, using compiled-in config", kConfigPath);
    return false;
  }

  File f = g_fs->open(kConfigPath, FILE_READ);
  if (!f) {
    LOG_WARN("SD: could not open %s", kConfigPath);
    return false;
  }

  size_t size = f.size();
  if (size == 0 || size > kMaxConfigBytes) {
    LOG_WARN("SD: %s has implausible size %u - ignoring", kConfigPath, static_cast<unsigned>(size));
    f.close();
    return false;
  }

  // +1 for the NUL the JSON parser needs.
  char* buf = static_cast<char*>(malloc(size + 1));
  if (!buf) {
    LOG_ERROR("SD: out of memory reading %s", kConfigPath);
    f.close();
    return false;
  }

  size_t read = f.readBytes(buf, size);
  buf[read] = '\0';
  f.close();

  char err[64] = {0};
  bool ok = rc_apply_json(buf, err, sizeof(err));
  free(buf);

  if (!ok) {
    // Deliberately non-fatal: a malformed card must not be able to stop the
    // device from booting on its compiled-in settings.
    LOG_ERROR("SD: %s is invalid (%s) - using compiled-in config", kConfigPath, err);
    return false;
  }

  LOG_INFO("SD: applied %u override(s) from %s", rc_override_count(), kConfigPath);
  return rc_has_overrides();
}

bool storage_append_history(time_t epoch, uint32_t uptime_s, float tempC, float rhPct,
                            float pressHPa, float battV, int battPct, int rssiDbm, float outTempC,
                            float outRhPct) {
  if (!g_mounted)
    return false;

  // On the internal partition, make room *before* the append: a write that dies
  // against a full filesystem leaves a truncated row, and 960 KB fills in about
  // two months at a 5-minute cadence. Bounded so a scan that frees nothing (only
  // nodate.csv left, remove failing) cannot loop.
  if (g_backend == Backend::kInternal) {
    int guard = 4;
    while (FFat.freeBytes() < kInternalFreeFloorBytes && guard-- > 0) {
      if (!remove_oldest_history())
        break;
    }
  }

  char path[48];
  history_path_for(epoch, path, sizeof(path));

  bool is_new = !g_fs->exists(path);

  File f = g_fs->open(path, FILE_APPEND);
  if (!f) {
    LOG_WARN("SD: cannot append to %s", path);
    return false;
  }

  if (is_new) {
    f.print(kHistoryHeader);
  }

  char iso[24];
  if (epoch_is_plausible(epoch)) {
    struct tm tm_local;
    localtime_r(&epoch, &tm_local);
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tm_local);
  } else {
    snprintf(iso, sizeof(iso), "unsynced");
  }

  // Non-finite readings are written as empty cells so a spreadsheet reads them
  // as missing rather than as 0 or "nan".
  auto write_float = [&f](float v, int decimals) {
    if (isfinite(v)) {
      f.print(v, decimals);
    }
    f.print(',');
  };

  f.print(iso);
  f.print(',');
  f.print(uptime_s);
  f.print(',');
  write_float(tempC, 2);
  write_float(rhPct, 1);
  write_float(pressHPa, 1);
  write_float(battV, 3);
  if (battPct >= 0) {
    f.print(battPct);
  }
  f.print(',');
  f.print(rssiDbm);
  f.print(',');
  write_float(outTempC, 2);
  if (isfinite(outRhPct)) {
    f.print(outRhPct, 0);
  }
  f.print('\n');

  f.close();
  return true;
}

// Parse one CSV data row. Columns:
// iso_time,uptime_s,temp_c,rh_pct,press_hpa,batt_v,batt_pct,rssi_dbm[,out_temp_c,out_rh_pct]
// Empty cells and missing trailing columns yield NAN.
static bool parse_history_row(const char* line, float* tempC, float* rhPct, float* outTempC,
                              float* outRhPct) {
  if (!line || !line[0] || strncmp(line, "iso_time", 8) == 0)
    return false;
  float cols[10];
  for (int i = 0; i < 10; i++) cols[i] = NAN;
  int col = 0;
  const char* p = line;
  while (col < 10) {
    if (*p != ',' && *p != '\0' && *p != '\n') {
      char* endp = nullptr;
      float v = strtof(p, &endp);
      if (endp != p)
        cols[col] = v;
    }
    const char* comma = strchr(p, ',');
    if (!comma)
      break;
    p = comma + 1;
    col++;
  }
  *tempC = cols[2];
  *rhPct = cols[3];
  *outTempC = cols[8];
  *outRhPct = cols[9];
  // A row with no finite inside temp is noise (or the header).
  return isfinite(*tempC) || isfinite(*rhPct);
}

static uint16_t backfill_one_file(const char* path, HistoryRowSink sink) {
  File f = g_fs->open(path, FILE_READ);
  if (!f)
    return 0;
  uint16_t rows = 0;
  char line[160];
  size_t len = 0;
  while (f.available()) {
    int ch = f.read();
    if (ch < 0)
      break;
    if (ch == '\n' || len >= sizeof(line) - 1) {
      line[len] = '\0';
      float t, r, ot, orh;
      if (parse_history_row(line, &t, &r, &ot, &orh)) {
        sink(t, r, ot, orh);
        rows++;
      }
      len = 0;
    } else if (ch != '\r') {
      line[len++] = static_cast<char>(ch);
    }
  }
  if (len > 0) {
    line[len] = '\0';
    float t, r, ot, orh;
    if (parse_history_row(line, &t, &r, &ot, &orh)) {
      sink(t, r, ot, orh);
      rows++;
    }
  }
  f.close();
  return rows;
}

uint16_t storage_backfill_history(HistoryRowSink sink) {
  if (!g_mounted || !sink)
    return 0;
  time_t now = time(nullptr);
  if (!epoch_is_plausible(now))
    return 0;  // no trustworthy clock, no way to pick the right files

  uint16_t rows = 0;
  char path[48];
  // Yesterday first so the sink receives samples in chronological order; the
  // ring keeps the newest HIST_CAP either way.
  history_path_for(now - 86400, path, sizeof(path));
  rows += backfill_one_file(path, sink);
  history_path_for(now, path, sizeof(path));
  rows += backfill_one_file(path, sink);
  if (rows > 0) {
    LOG_INFO("Storage: backfilled %u history rows into the ring", rows);
  }
  return rows;
}

uint16_t storage_prune_history(uint16_t retention_days) {
  if (!g_mounted || retention_days == 0)
    return 0;

  time_t now = time(nullptr);
  if (!epoch_is_plausible(now))
    return 0;  // cannot judge age without a trustworthy clock

  long today = static_cast<long>(now / 86400);
  uint16_t removed = 0;

  File dir = g_fs->open(kDataDir);
  if (!dir)
    return 0;

  // Collect names first: deleting while iterating a FatFs directory handle is
  // not guaranteed to be safe. The batch is bounded to keep this off the stack
  // budget; a backlog larger than one batch is cleared over successive sweeps.
  constexpr uint8_t kMaxPerSweep = 16;
  char doomed[kMaxPerSweep][48];
  uint8_t doomed_count = 0;
  bool hit_batch_cap = false;

  File entry = dir.openNextFile();
  while (entry) {
    if (doomed_count >= kMaxPerSweep) {
      hit_batch_cap = true;
      entry.close();
      break;
    }
    if (!entry.isDirectory()) {
      const char* full = entry.name();
      // File::name() may return either a bare name or a full path depending on
      // core version; compare against the last path segment either way.
      const char* base = strrchr(full, '/');
      base = base ? base + 1 : full;

      long file_day = 0;
      if (history_day_from_name(base, &file_day) && (today - file_day) > retention_days) {
        snprintf(doomed[doomed_count], sizeof(doomed[doomed_count]), "%s/%s", kDataDir, base);
        doomed_count++;
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  if (entry)
    entry.close();
  dir.close();

  for (uint8_t i = 0; i < doomed_count; i++) {
    if (g_fs->remove(doomed[i])) {
      removed++;
      LOG_INFO("SD: pruned %s", doomed[i]);
    }
  }
  if (hit_batch_cap) {
    LOG_INFO("SD: prune batch cap (%u) reached; remaining files go next sweep", kMaxPerSweep);
  }
  return removed;
}

bool storage_log_write(const char* line) {
  if (!g_mounted || !line)
    return false;

  // Same storage-full guard as history appends: on the internal partition,
  // reclaim space before writing rather than failing mid-append.
  if (g_backend == Backend::kInternal) {
    int guard = 4;
    while (FFat.freeBytes() < kInternalFreeFloorBytes && guard-- > 0) {
      if (!remove_oldest_history())
        break;
    }
    if (FFat.freeBytes() < kInternalFreeFloorBytes / 2)
      return false;  // absolute floor: drop the log line, never wedge the FS
  }

  load_log_index();

  char path[32];
  log_path_for(g_log_index, path, sizeof(path));

  File f = g_fs->open(path, FILE_APPEND);
  if (!f)
    return false;

  // The compile-time rotation size is tuned for a multi-GB card; on the 920 KB
  // internal partition that budget (5 x 256 KB) exceeds the whole filesystem
  // and would wedge it. Scale the per-file cap to the backend.
  const size_t log_max =
      (g_backend == Backend::kInternal) ? (24 * 1024) : static_cast<size_t>(SD_LOG_MAX_BYTES);
  if (f.size() >= log_max) {
    f.close();
    g_log_index = (g_log_index + 1) % SD_LOG_FILE_COUNT;
    log_path_for(g_log_index, path, sizeof(path));
    // Starting a fresh generation: drop whatever the oldest file held.
    g_fs->remove(path);
    save_log_index();
    f = g_fs->open(path, FILE_APPEND);
    if (!f)
      return false;
  }

  // Timestamp here rather than at the call sites: this is the one choke point,
  // and a log whose lines cannot be placed in time is not worth the write.
  time_t now = time(nullptr);
  char stamp[24];
  if (epoch_is_plausible(now)) {
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm_local);
  } else {
    // Pre-NTP lines still need to be orderable, so fall back to uptime.
    snprintf(stamp, sizeof(stamp), "+%lus", static_cast<unsigned long>(millis() / 1000));
  }

  f.print(stamp);
  f.print(' ');
  f.println(line);
  f.close();
  return true;
}

bool storage_has_staged_update() { return g_mounted && g_fs->exists(SD_UPDATE_PATH); }

File storage_open_staged_update(size_t* size_out) {
  if (size_out)
    *size_out = 0;
  if (!g_mounted)
    return File();

  File f = g_fs->open(SD_UPDATE_PATH, FILE_READ);
  if (f && size_out)
    *size_out = f.size();
  return f;
}

void storage_finish_staged_update(bool success) {
  if (!g_mounted)
    return;

  const char* target = success ? SD_UPDATE_APPLIED_PATH : SD_UPDATE_FAILED_PATH;
  // rename() will not overwrite, so clear any result from a previous attempt.
  g_fs->remove(target);
  if (!g_fs->rename(SD_UPDATE_PATH, target)) {
    // If the rename fails the image would be retried forever, so remove it.
    LOG_WARN("SD: could not rename staged image to %s - deleting it", target);
    g_fs->remove(SD_UPDATE_PATH);
  }
}

#else  // !FEATURE_STORAGE

static StorageInfo g_disabled_info;

bool storage_begin() { return false; }
void storage_end() {}
bool storage_is_mounted() { return false; }
fs::FS* storage_fs() { return nullptr; }
const StorageInfo& storage_get_info() { return g_disabled_info; }
bool storage_load_config() { return false; }
bool storage_append_history(time_t, uint32_t, float, float, float, float, int, int, float, float) {
  return false;
}
uint16_t storage_backfill_history(HistoryRowSink) { return 0; }
uint16_t storage_prune_history(uint16_t) { return 0; }
bool storage_log_write(const char*) { return false; }
bool storage_has_staged_update() { return false; }
File storage_open_staged_update(size_t* size_out) {
  if (size_out)
    *size_out = 0;
  return File();
}
void storage_finish_staged_update(bool) {}

#endif  // FEATURE_STORAGE

// --- serial log mirror -------------------------------------------------------
// Deliberately outside the feature guard: logging.h declares log_mirror() for
// every translation unit that logs, so exactly one definition must exist whether
// or not the SD library is compiled in. With FEATURE_STORAGE=0 the mirror can
// never be enabled and storage_log_write() is a stub, so this costs one branch.

namespace {
bool g_mirror_enabled = false;
// log_mirror() is reached from inside storage's own LOG_WARN/LOG_ERROR calls
// (storage_begin and storage_prune_history both log). Those do not recurse today, but a
// logging call added to the write path later would loop until the stack ran out,
// which is a bad way to find out.
bool g_mirror_active = false;
}  // namespace

void storage_log_set_mirror(bool enabled) { g_mirror_enabled = enabled; }

bool storage_log_mirror_enabled() { return g_mirror_enabled; }

void log_mirror(const char* level, const char* fmt, ...) {
  if (!g_mirror_enabled || g_mirror_active || !storage_is_mounted())
    return;

  g_mirror_active = true;

  // Sized to match Logger::MAX_MESSAGE_LENGTH plus the level tag; anything
  // longer is truncated rather than heap-allocated on a logging path.
  char line[160];
  int n = snprintf(line, sizeof(line), "[%s] ", level);
  if (n > 0 && static_cast<size_t>(n) < sizeof(line)) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, args);
    va_end(args);
  }

  storage_log_write(line);

  g_mirror_active = false;
}
