#pragma once
// Copyright 2024 Justin
//
// microSD storage on the eInk FeatherWing.
//
// The card serves four jobs:
//   /config/device.json  runtime config overrides (see runtime_config.h)
//   /data/YYYY-MM-DD.csv one row per sample, for local long-term history
//   /logs/logN.txt       rotating log sink; NVS only holds the last 50 entries
//   /firmware/update.bin  a firmware image to apply at boot (see ota_manager.h)
//
// The card is optional. When it is absent, unreadable, or the feature is
// compiled out, every function here degrades to a no-op and the device runs
// exactly as it did before — nothing in the boot path is allowed to depend on
// storage being present.
//
// Note the SPI bus is shared with the e-ink panel (panel CS = D9, card CS = D5),
// so sd_begin() must run after the display has initialised the bus.

#include <Arduino.h>
#include <FS.h>
#include <time.h>
#include "config.h"
#include "feature_flags.h"

struct SdInfo {
  bool mounted = false;
  uint64_t total_bytes = 0;
  uint64_t used_bytes = 0;
  const char* type = "none";
  // Populated when a mount attempt fails, for surfacing over MQTT/serial.
  char last_error[48] = {0};
};

// Mount the card. Idempotent: returns true immediately if already mounted.
bool sd_begin();
void sd_end();
bool sd_is_mounted();
const SdInfo& sd_get_info();

// Read /config/device.json and overlay it onto the runtime config. Returns true
// only when a file was found, parsed and applied. Absent file is not an error.
bool sd_load_config();

// Append one sample to today's CSV, creating the file with a header when new.
// `epoch` selects the file name and timestamp column; pass 0 when time is not
// yet known and rows are routed to /data/nodate.csv instead of being dropped.
bool sd_append_history(time_t epoch, uint32_t uptime_s, float tempC, float rhPct, float pressHPa,
                       float battV, int battPct, int rssiDbm);

// Delete /data CSVs older than `retention_days`. 0 means keep everything.
// Returns the number of files removed.
uint16_t sd_prune_history(uint16_t retention_days);

// Append a line to the rotating log, rotating first if the active file is full.
bool sd_log_write(const char* line);

// --- staged firmware image ---------------------------------------------------
// Path constants are exposed so ota_manager can report them without duplicating
// string literals.
#define SD_UPDATE_PATH "/firmware/update.bin"
#define SD_UPDATE_APPLIED_PATH "/firmware/update.applied"
#define SD_UPDATE_FAILED_PATH "/firmware/update.failed"

bool sd_has_staged_update();
// Open the staged image for reading. Caller must close the returned File.
File sd_open_staged_update(size_t* size_out);
// Rename the staged image so it is not retried on the next boot.
void sd_finish_staged_update(bool success);
