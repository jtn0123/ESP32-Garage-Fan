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
//
// Transport is PLAIN HTTP -- an accepted integrity tradeoff, not an
// oversight. TLS was implemented first and failed on hardware: mbedtls needs
// ~34 KB of contiguous internal RAM that this S2 cannot produce ("SSL -
// Memory allocation failed", 2026-08-10). An on-path forger can therefore
// feed a fake temperature; the blast radius is a fan speed, bounded by the
// plausibility clamp and by auto's one-step-per-tick ramp, and the MQTT
// outdoor feed remains an independent second source. Anything needing a
// stronger guarantee must NOT reuse this transport.

namespace weather {

/** True when coordinates were baked in and the poller is active. */
bool enabled();

/**
 * Rate-limited to one fetch per 10 min; call at any convenient cadence once
 * WiFi is up. Blocks the loop for the fetch (measured ~0.5 s over plain
 * HTTP; hard-capped by the 10 s connect/read timeouts), which the 60 s MQTT
 * keepalive and watchdog both tolerate; the duration is logged whenever it
 * exceeds 5 s, so a slow drift shows up on the tape rather than as another
 * mystery stall.
 */
void tick();

}  // namespace weather
