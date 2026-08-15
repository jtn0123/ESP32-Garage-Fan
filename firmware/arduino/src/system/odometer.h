#pragma once
// Runtime odometer: seconds run today and lifetime, plus the energy estimate
// (cubic fan law via fan::watts). Persists to NVS every 15 minutes so a
// reboot loses at most that much accounting, never the lifetime total.

#include <Preferences.h>
#include <stdint.h>

namespace odometer {

/** Load persisted counters and remember where to flush them. */
void restore(Preferences* prefs);

/**
 * Call every loop. Internally rate-limited: integrates run time and energy
 * once per second while `speed > 0`, rolls the daily counter at local
 * midnight once the clock is synced, and flushes to NVS every 15 minutes.
 */
void tick(int speed, float watts);

uint32_t run_today_s();
uint32_t run_total_s();
float energy_wh();
/** Watt-hours since local midnight (measured watts when the meter answers). */
float wh_today();

/** Electricity price, $/kWh, for the console's cost readouts. */
float cost_per_kwh();
void set_cost_per_kwh(float v);

}  // namespace odometer
