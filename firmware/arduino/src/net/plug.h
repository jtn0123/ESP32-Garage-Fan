#pragma once
// The Tapo P110M watt meter on the fan's supply, polled out of Home
// Assistant. This is the fan link's ONLY feedback: the control wire is
// fire-and-forget (no tach, no ack -- PROTOCOL.md), and on 2026-08-13 the fan
// ran at full power for a day while everything on this device honestly said
// OFF. A watt reading that disagrees with the commanded speed is the one
// signal that can catch that class of failure.
//
// Why via HA and not the plug directly: the P110M's native protocol is
// "TPAP", newer than anything python-kasa or any open client speaks; the plug
// is Matter-commissioned into HA, and HA's REST API is the supported way to
// read it. The token comes from the gitignored .env, like the WiFi
// credentials; a clone without HA_URL/HA_TOKEN builds with this module
// disabled. Transport is plain HTTP on the LAN for the same measured mbedtls
// reason as net/weather.h.
#include <stddef.h>
#include <stdint.h>

namespace plug {

/** True when HA_URL and HA_TOKEN were baked in and the poller is active. */
bool enabled();

/** Rate-limited poll; call from the loop once WiFi is up. */
void tick();

/** Latest measured draw in watts, or NAN before the first good read. */
float watts();

/** Latest mains voltage, or NAN. */
float volts();

/** Seconds since the last good read, or -1 before the first. */
int32_t age_s();

/**
 * Does the measured draw agree with the commanded speed?
 *  +1 agree, -1 DISAGREE (sustained), 0 cannot say (stale reading, speed
 * changed too recently, or the poller is disabled).
 *
 * The band is generous on purpose -- adjacent speeds overlap at the low end
 * of the table and mains voltage moves the draw a few percent. This exists to
 * catch "commanded 0, pulling 40 W", not to grade the calibration.
 */
int verdict();

/** Expected draw for a speed, from the measured baseline table. */
float expected_w(int speed);

/**
 * The cycling profile (net/plug_cycle.h): true while the meter shows the fan
 * switching itself on and off -- four or more confirmed run/stop flips in ten
 * minutes -- with ONE speed held the whole time. Distinct from verdict -1,
 * which is "steadily wrong": on 2026-08-20 the fan cycled all night at a
 * held speed 10 and the verdict only flapped.
 */
bool cycling();

/** Confirmed run/stop flips inside the last ten minutes; 0 when steady. */
uint8_t flips();

/**
 * Flips counted since the previous call -- the 5-minute history row's
 * `flips` column, so the chart can show cycling the snapshot aliases away.
 * -1 when the meter is disabled or has never answered.
 */
int8_t take_bucket_flips();

/**
 * The retained kTopicAlert payload for the CURRENT verdict:
 * {"kind":"plug_disagree","speed":N,"expect_w":X,"measured_w":Y} or
 * {"kind":"ok"}. Published on both verdict edges and re-asserted on every
 * broker connect, so the retained value never outlives the condition.
 */
void alert_json(char* out, size_t cap);

}  // namespace plug
