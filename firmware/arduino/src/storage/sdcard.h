#pragma once
// The microSD card: monthly CSV logs behind the 7-day and 30-day chart
// ranges. Owns the mount state, the crash quarantine, and the retry backoff.
//
// Hard-won rules encoded here (see the crashlog sentinel for the receipts):
//  * SD stays OUT of the boot path. First mount attempt fires from the loop
//    the moment WiFi associates (freshest post-radio heap; the mount needs
//    one contiguous ~13 KB block and fragmentation only grows from there) --
//    a card that crashes the mount gets quarantined and can never take fan
//    control down with it.
//  * Every mount and write is bracketed by the crashlog breadcrumb, so a
//    death inside an SD op is attributable on the next boot.
//  * Formatting is never automatic. A flaky-but-full card must not be
//    silently erased; format() exists only behind the token-guarded endpoint.

#include <stdint.h>

#include <ctime>

namespace sdcard {

/** Mounted and answering? */
bool ok();

/** Quarantined after a previous boot died inside an SD op. */
bool quarantined();
void set_quarantined(bool q);

/** Mount attempt wrapped in the crashlog sentinel. */
void mount_guarded();

/**
 * The raw probe endpoint tears down the SD/SPI buses behind this module's
 * back; it must declare that here so the mount state cannot go stale and
 * silently drop CSV rows until the retry backoff catches up.
 */
void mark_unmounted();

/**
 * Loop-cadence retry: first attempt when WiFi comes up (60 s fallback), then
 * every 10th call (~5 min at the auto tick), capped at 10 failures, never
 * while quarantined. /api/sdformat overrides all of it.
 */
void retry_tick();

/**
 * WiFi just associated: mount now, while the post-radio heap still has a
 * contiguous block big enough for the filesystem (see retry_tick's comment).
 */
void on_network_up();

/**
 * Append one sample row to this month's CSV. Drops the mount on failure.
 *
 * Row format:
 * epoch,temp_c,rh,hpa,outside_f,speed,batt_v,chg,watts,voc_raw,nox_raw,voc,nox batt_v
 * and chg were appended 1.14.23. Rows written before that carry six fields; read_range parses both
 * widths and reports the missing columns as absent rather than as zero.
 */
void log_sample(time_t now, float t, float h, float p, float out_f, int speed, float batt_v,
                int chg, float watts, int32_t voc_raw, int32_t nox_raw, int voc, int nox);

/**
 * Append one flight-recorder line (newline added) to /events.log, rotating
 * to /events.old at ~512 KB. False -- and the mount dropped, like
 * log_sample -- when the write fails.
 */
bool append_event_line(const char* line);

/** Copy the tail of /events.log into buf, NUL-terminated. Bytes copied. */
uint32_t tail_events(char* buf, uint32_t cap);

/**
 * Destination for read_range. Every pointer must have room for max_pts.
 *
 * `ts` is the reason this struct exists. read_range used to return only
 * temp/rh/hpa and no timestamps at all, so /api/history's 7- and 30-day
 * responses carried neither an anchor nor the other four series. The console
 * then fell back to index-proportional spacing and drew whatever rows existed
 * evenly across the whole requested window -- two days of data stretched over
 * a 30-day axis, with gap detection permanently disabled because every step
 * measured exactly "one interval". Real per-row epochs make the axis true and
 * outages visible.
 */
struct Samples {
  time_t* ts;
  float* temp_c;
  float* rh;
  float* hpa;
  float* out_f;   // NAN when the row recorded no yard reading
  float* batt_v;  // NAN on pre-1.14.23 rows
  int8_t* spd;
  int8_t* chg;       // 1 charging, 0 not, -1 unknown
  float* watts;      // NAN on pre-1.14.47 rows
  int32_t* voc_raw;  // -1 on pre-1.14.47 rows
  int32_t* nox_raw;
  int16_t* voc;
  int16_t* nox;
};

/**
 * Read [cutoff, now] from the month files, decimated by stride into `out`.
 * Returns rows kept.
 */
uint16_t read_range(time_t cutoff, const Samples& out, uint16_t max_pts);

/** Free space in MB, 0 while unmounted. */
uint32_t free_mb();

/** Result of purge(). */
struct PurgeResult {
  uint32_t files = 0;  // entries unlinked
  uint32_t dirs = 0;   // directories removed
  uint32_t freed_mb = 0;
  uint32_t remaining = 0;     // foreign entries still in root (excludes our logs)
  uint32_t skipped_dirs = 0;  // directories the queue bound could not hold
  uint32_t too_long = 0;      // entries skipped because their path did not fit
  bool complete = false;      // nothing left to delete and no bound was hit
  /**
   * Name of the first entry that survived, "" if none.
   *
   * Reported because "some entries could not be deleted" is not actionable on
   * its own. In practice this is `.Spotlight-V100`, the metadata stub macOS
   * leaves on a card it has mounted; FAT will not let it be removed and
   * repeating the purge cannot change that, so the caller needs to see WHAT
   * is left before deciding whether to care.
   */
  char leftover[48] = {0};
};

/**
 * Delete everything on the card WITHOUT reformatting it.
 *
 * The deployed card reported 28677 MB used of 28887 -- 99.3% full, 210 MB free
 * -- while this firmware's own logs came to 308 bytes. The bulk is data from
 * whatever the card did before, and format() never removed it: SD.begin's
 * format_if_empty only formats a card that fails to MOUNT, so a full,
 * healthy card was remounted in 166 ms and reported "ok" with every byte
 * still on it.
 *
 * This unlinks the contents instead, leaving the existing filesystem in place.
 * Destructive and irreversible: token-guarded, and the console confirms.
 */
PurgeResult purge();

/** Receives one CSV row (no newline). Return false to stop the stream. */
using LineSink = bool (*)(const char* line, void* ctx);

/**
 * Stream every stored row in [cutoff, now] to `sink`, undecimated.
 *
 * This is what /download.csv needs and could not get: that endpoint used to
 * serialise the in-RAM ring, so "download my logged data" handed back the
 * ~7 rows since the last reboot (308 bytes measured on the live device) while
 * a month of samples sat on the card with no way to reach them short of
 * pulling it. read_range cannot serve this -- it decimates to fit a chart.
 *
 * Returns rows emitted.
 */
uint32_t stream_range(time_t cutoff, LineSink sink, void* ctx);

/**
 * Token-verified caller only: full teardown, then mount with
 * format-on-failure (a fresh or exFAT card becomes FAT32 in place). Clears
 * the quarantine and the failure count. True if the card MOUNTED -- which is
 * not the same as "was erased", and the difference matters:
 *
 *   This CANNOT wipe a card that already carries a mountable filesystem.
 *   SD.begin's flag is format_if_EMPTY, so a card with a valid FAT returns
 *   in milliseconds with all its data intact. To actually erase a working
 *   card, pull it and format it on a computer.
 *
 * The endpoint reports used_before/used_mb so a no-op cannot masquerade as a
 * wipe (it did exactly that on a 99%-full 28 GB card, 2026-08-09).
 */
bool format();

uint32_t total_mb();
uint32_t used_mb();

}  // namespace sdcard
