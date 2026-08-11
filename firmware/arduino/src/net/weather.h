#pragma once
// Direct outdoor-temperature poll: the fan asks open-meteo.com itself, every
// 10 minutes, instead of trusting a chain of Home Assistant automations to
// relay the yard temperature over MQTT.
//
// Why this module exists: auto mode was blind for FIVE DAYS (2026-08-05..10)
// because the HA automation feeding home/outdoor/temp_f silently died -- it
// kept "firing" while an orphaned entity reference made it publish nothing.
// Then the repaired feed turned out to live behind a configuration include
// that HA never loads. Every link in that chain is outside this repo's
// control and fails silently; a direct poll fails loudly, on our own tape,
// and the MQTT path remains as an independent second source (both feed
// climate::set_outside_f; freshest write wins).
//
// The device's coordinates come from the gitignored .env (WEATHER_LAT/LON),
// like credentials: a clone without them builds with the poller disabled.
// TLS is pinned to ISRG Root X1, the root open-meteo's chain terminates in.

namespace weather {

/** True when coordinates were baked in and the poller is active. */
bool enabled();

/**
 * Rate-limited to one fetch per 10 min; call at any convenient cadence once
 * WiFi is up. Blocks the loop for the fetch (~1-3 s of TLS on this S2), which
 * the 60 s MQTT keepalive and watchdog both tolerate; the duration is logged
 * whenever it exceeds 5 s, so a slow drift shows up on the tape rather than
 * as another mystery stall.
 */
void tick();

}  // namespace weather
