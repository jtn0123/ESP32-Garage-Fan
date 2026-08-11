#pragma once
// The daily-runtime accounting, split out so it can be tested on the host.
//
// Same reason auto_logic.h and battery_logic.h exist: the decision is small,
// the consequences are user-visible, and the surrounding module is welded to
// NVS and the system clock. Two bugs lived in here, both invisible without a
// test:
//
//   1. The day key came from gmtime_r against a UTC clock, so the counter the
//      console labels "FAN TODAY" reset at 17:00 local -- in the middle of the
//      afternoon the fan works hardest.
//   2. Elapsed time was credited BEFORE the rollover was processed, so a tick
//      that spanned local midnight had its post-midnight seconds zeroed along
//      with the rest. This function credits whole accumulated spans (a loop
//      stalled in an SD format must not count as one second), so that span can
//      be minutes.

#include <stdint.h>

namespace odometer_logic {

/** What one tick should add, once the calendar has been consulted. */
struct Credit {
  uint32_t total_s;  // added to the lifetime counter
  uint32_t today_s;  // added to the daily counter, AFTER any reset
  bool reset_today;  // zero the daily counter before adding today_s
};

/**
 * Split `elapsed_s` across a local-midnight boundary.
 *
 * @param elapsed_s        whole seconds since the last tick
 * @param running          was the fan on for that span
 * @param clock_valid      has SNTP synced (no calendar reasoning without it)
 * @param ymd              today's local date key, YYYYMMDD
 * @param prev_ymd         the key this counter was last credited under, 0 if none
 * @param since_midnight_s local seconds elapsed today, i.e. h*3600+m*60+s
 */
inline Credit credit(uint32_t elapsed_s, bool running, bool clock_valid, uint32_t ymd,
                     uint32_t prev_ymd, uint32_t since_midnight_s) {
  Credit c{0, 0, false};
  if (!running)
    return c;  // the clock still advances; the counters do not
  c.total_s = elapsed_s;
  c.today_s = elapsed_s;
  if (!clock_valid)
    return c;
  if (prev_ymd != 0 && ymd != prev_ymd) {
    c.reset_today = true;
    // Only the part of this span that lies on the new day belongs to it. The
    // rest belonged to yesterday and is gone with the reset -- which is
    // correct; what was wrong was throwing away this part too.
    c.today_s = since_midnight_s < elapsed_s ? since_midnight_s : elapsed_s;
  }
  return c;
}

}  // namespace odometer_logic
