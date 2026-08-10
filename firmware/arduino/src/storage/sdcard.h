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

/** Append one sample row to this month's CSV. Drops the mount on failure. */
void log_sample(time_t now, float t, float h, float p, float out_f, int speed);

/**
 * Append one flight-recorder line (newline added) to /events.log, rotating
 * to /events.old at ~512 KB. False -- and the mount dropped, like
 * log_sample -- when the write fails.
 */
bool append_event_line(const char* line);

/** Copy the tail of /events.log into buf, NUL-terminated. Bytes copied. */
uint32_t tail_events(char* buf, uint32_t cap);

/**
 * Read [cutoff, now] from the month files, decimated by stride into the
 * caller's arrays. Returns rows kept.
 */
uint16_t read_range(time_t cutoff, float* t, float* h, float* p, uint16_t max_pts);

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
