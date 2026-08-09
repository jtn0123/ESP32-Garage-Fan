#pragma once
// WiFi custody: bring-up, the disconnect ledger, and the self-heal ladder.
// Exists because the random-disconnect investigation needs every drop on the
// record with its reason code, and because a deployed board with no keyboard
// must climb back onto the network by itself:
//
//   drop        -> the core's auto-reconnect tries first (explicitly on);
//   60 s down   -> a WiFi.reconnect() nudge, once a minute, logged;
//   15 min down -> reboot. A power cycle is the one reliably curative move
//                  for a wedged ESP32 radio/IP stack, the reboot itself is
//                  logged (and flushed to SD when possible), and the fan
//                  resumes its NVS speed on the way back up.

#include <stdint.h>

namespace wifi_link {

/** Radio up, credentials in, SNTP kicked off. Call once from setup(). */
void begin();

/** The nudge/reboot ladder above. Loop cadence; cheap while connected. */
void tick();

bool connected();

/** STA disconnect events since boot (the "drops" field on /api/state). */
uint32_t drops();

/** Current RSSI in dBm, or 0 while disconnected. */
int rssi();

}  // namespace wifi_link
