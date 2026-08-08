#pragma once
// The microSD card: monthly CSV logs behind the 7-day and 30-day chart
// ranges. Owns the mount state, the crash quarantine, and the retry backoff.
//
// Hard-won rules encoded here (see the crashlog sentinel for the receipts):
//  * SD stays OUT of the boot path. First mount attempt is ~60 s after boot,
//    from the loop -- a card that crashes the mount gets quarantined and can
//    never take fan control down with it.
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
 * Loop-cadence retry: first attempt ~60 s after boot, then every 10th call
 * (~5 min at the auto tick), capped at 10 failures, never while quarantined.
 * /api/sdformat overrides all of it.
 */
void retry_tick();

/** Append one sample row to this month's CSV. Drops the mount on failure. */
void log_sample(time_t now, float t, float h, float p, float out_f, int speed);

/**
 * Read [cutoff, now] from the month files, decimated by stride into the
 * caller's arrays. Returns rows kept.
 */
uint16_t read_range(time_t cutoff, float* t, float* h, float* p, uint16_t max_pts);

/**
 * Token-verified caller only: full teardown, then mount with
 * format-on-failure (fresh exFAT card becomes FAT32 in place). Blocks for
 * minutes on big cards; the RMT peripheral keeps the fan running throughout.
 * Clears the quarantine and the failure count. True if the card mounted.
 */
bool format();

uint32_t total_mb();
uint32_t used_mb();

}  // namespace sdcard
