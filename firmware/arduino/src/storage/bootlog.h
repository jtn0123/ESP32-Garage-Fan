#pragma once
// One line per boot, so a hole in the charts can say WHY it is a hole.
//
// The climate CSV records samples; it cannot record its own absence. A reboot
// costs 12-16 minutes of rows (mount, SNTP, first sample) and the console had
// no way to tell that outage apart from a wedged card or a dead sensor -- the
// chart drew an honest gap and left the operator to guess. This file is the
// missing half: `epoch,boots,cause`, appended once per boot, so the gap can
// be labelled "restarted here (brownout)" instead of shrugging.
//
// Written only AFTER the clock syncs -- an unstamped boot line would be worse
// than none, and the boot's own epoch is reconstructed as now - uptime.

#include <stdint.h>

#include <ctime>

#include "storage/sdcard.h"

namespace bootlog {

/**
 * Call every loop. Does nothing until the clock is synced and the card is
 * mounted, writes exactly one line per boot, then does nothing forever.
 */
void tick();

/** Stream `epoch,boots,cause` lines at or after `cutoff`, oldest first. */
uint32_t stream(time_t cutoff, sdcard::LineSink sink, void* ctx);

}  // namespace bootlog
