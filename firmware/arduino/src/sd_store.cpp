// microSD storage implementation
// Copyright 2024 Justin

#include "sd_store.h"

#include <math.h>
#include <time.h>
#include <cstdio>

// SD.h/SPI.h stay behind the feature guard so a FEATURE_SD_STORAGE=0 build does
// not pull the SD library into the link at all.
#if FEATURE_SD_STORAGE

#include <SD.h>
#include <SPI.h>
#include "logging.h"
#include "safe_strings.h"
#include "runtime_config.h"

// At file scope rather than in the anonymous namespace below: cpplint reads the
// wrapped continuation line as an indented namespace body.
static constexpr const char* kHistoryHeader =
    "iso_time,uptime_s,temp_c,rh_pct,press_hpa,batt_v,batt_pct,rssi_dbm\n";

namespace {

constexpr const char* kConfigPath = "/config/device.json";
constexpr const char* kDataDir = "/data";
constexpr const char* kLogDir = "/logs";
constexpr const char* kFirmwareDir = "/firmware";
constexpr const char* kLogIndexPath = "/logs/index.txt";

// The config file is a handful of keys; refuse anything suspiciously large
// rather than trying to allocate it.
constexpr size_t kMaxConfigBytes = 4096;

SdInfo g_info;
bool g_mounted = false;
uint8_t g_log_index = 0;
bool g_log_index_loaded = false;

void set_error(const char* msg) { safe_strcpy(g_info.last_error, msg); }

// A year below 2020 means NTP has not landed yet and the RTC is at its epoch
// default; timestamps derived from it would be worse than useless.
bool epoch_is_plausible(time_t epoch) {
  return epoch > 1577836800;  // 2020-01-01T00:00:00Z
}

void ensure_dir(const char* path) {
  if (!SD.exists(path)) {
    SD.mkdir(path);
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

  File f = SD.open(kLogIndexPath, FILE_READ);
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
  File f = SD.open(kLogIndexPath, FILE_WRITE);
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

}  // namespace

bool sd_begin() {
  if (g_mounted)
    return true;

  g_info = SdInfo();

  // The SD driver only ever calls beginTransaction(); it never starts the bus
  // itself. With a display build GxEPD2 has already done so, but the headless
  // build has no other SPI user, so do it here. SPIClass::begin() early-returns
  // when the bus is already up, which makes this safe in both cases — and it is
  // what lets the card share the bus with the panel (panel CS D9, card CS D5).
  SPI.begin();

  if (!SD.begin(SD_CS_PIN, SPI, SD_SPI_FREQ_HZ)) {
    set_error("SD.begin failed (card absent?)");
    LOG_WARN("SD: mount failed on CS=%d - continuing without card", SD_CS_PIN);
    return false;
  }

  uint8_t card_type = SD.cardType();
  if (card_type == CARD_NONE) {
    SD.end();
    set_error("no card detected");
    LOG_WARN("SD: no card detected");
    return false;
  }

  switch (card_type) {
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
  g_info.mounted = true;
  g_mounted = true;

  ensure_dir(kDataDir);
  ensure_dir(kLogDir);
  ensure_dir(kFirmwareDir);

  LOG_INFO("SD: mounted %s, %llu MB total, %llu MB used", g_info.type,
           g_info.total_bytes / (1024ULL * 1024ULL), g_info.used_bytes / (1024ULL * 1024ULL));
  return true;
}

void sd_end() {
  if (!g_mounted)
    return;
  SD.end();
  g_mounted = false;
  g_info.mounted = false;
}

bool sd_is_mounted() { return g_mounted; }

const SdInfo& sd_get_info() { return g_info; }

bool sd_load_config() {
  if (!g_mounted)
    return false;

  if (!SD.exists(kConfigPath)) {
    LOG_INFO("SD: no %s, using compiled-in config", kConfigPath);
    return false;
  }

  File f = SD.open(kConfigPath, FILE_READ);
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

bool sd_append_history(time_t epoch, uint32_t uptime_s, float tempC, float rhPct, float pressHPa,
                       float battV, int battPct, int rssiDbm) {
  if (!g_mounted)
    return false;

  char path[48];
  history_path_for(epoch, path, sizeof(path));

  bool is_new = !SD.exists(path);

  File f = SD.open(path, FILE_APPEND);
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
  f.print('\n');

  f.close();
  return true;
}

uint16_t sd_prune_history(uint16_t retention_days) {
  if (!g_mounted || retention_days == 0)
    return 0;

  time_t now = time(nullptr);
  if (!epoch_is_plausible(now))
    return 0;  // cannot judge age without a trustworthy clock

  long today = static_cast<long>(now / 86400);
  uint16_t removed = 0;

  File dir = SD.open(kDataDir);
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
    if (SD.remove(doomed[i])) {
      removed++;
      LOG_INFO("SD: pruned %s", doomed[i]);
    }
  }
  if (hit_batch_cap) {
    LOG_INFO("SD: prune batch cap (%u) reached; remaining files go next sweep", kMaxPerSweep);
  }
  return removed;
}

bool sd_log_write(const char* line) {
  if (!g_mounted || !line)
    return false;

  load_log_index();

  char path[32];
  log_path_for(g_log_index, path, sizeof(path));

  File f = SD.open(path, FILE_APPEND);
  if (!f)
    return false;

  if (f.size() >= SD_LOG_MAX_BYTES) {
    f.close();
    g_log_index = (g_log_index + 1) % SD_LOG_FILE_COUNT;
    log_path_for(g_log_index, path, sizeof(path));
    // Starting a fresh generation: drop whatever the oldest file held.
    SD.remove(path);
    save_log_index();
    f = SD.open(path, FILE_APPEND);
    if (!f)
      return false;
  }

  f.println(line);
  f.close();
  return true;
}

bool sd_has_staged_update() { return g_mounted && SD.exists(SD_UPDATE_PATH); }

File sd_open_staged_update(size_t* size_out) {
  if (size_out)
    *size_out = 0;
  if (!g_mounted)
    return File();

  File f = SD.open(SD_UPDATE_PATH, FILE_READ);
  if (f && size_out)
    *size_out = f.size();
  return f;
}

void sd_finish_staged_update(bool success) {
  if (!g_mounted)
    return;

  const char* target = success ? SD_UPDATE_APPLIED_PATH : SD_UPDATE_FAILED_PATH;
  // rename() will not overwrite, so clear any result from a previous attempt.
  SD.remove(target);
  if (!SD.rename(SD_UPDATE_PATH, target)) {
    // If the rename fails the image would be retried forever, so remove it.
    LOG_WARN("SD: could not rename staged image to %s - deleting it", target);
    SD.remove(SD_UPDATE_PATH);
  }
}

#else  // !FEATURE_SD_STORAGE

static SdInfo g_disabled_info;

bool sd_begin() { return false; }
void sd_end() {}
bool sd_is_mounted() { return false; }
const SdInfo& sd_get_info() { return g_disabled_info; }
bool sd_load_config() { return false; }
bool sd_append_history(time_t, uint32_t, float, float, float, float, int, int) { return false; }
uint16_t sd_prune_history(uint16_t) { return 0; }
bool sd_log_write(const char*) { return false; }
bool sd_has_staged_update() { return false; }
File sd_open_staged_update(size_t* size_out) {
  if (size_out)
    *size_out = 0;
  return File();
}
void sd_finish_staged_update(bool) {}

#endif  // FEATURE_SD_STORAGE
