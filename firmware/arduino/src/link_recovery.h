#pragma once
// Copyright 2026 Justin
//
// Link-health recovery policy for the always-on node, as a pure value type.
//
// The problem this owns (observed on hardware 2026-08-01): the node can come up
// "connected" -- core 3.x wifi_is_connected() means associated AND holding a
// DHCP lease -- yet be unreachable from its own subnet, with MQTT never
// connecting. Recovery gated on !wifi_is_connected() can therefore never fire,
// and the node stays dark until someone reaches it physically. The broker sits
// on the same LAN, so sustained broker failure while the link claims health is
// treated as the link lying, and remedies escalate: cycle the radio (forces a
// fresh association and DHCP exchange), then reboot.
//
// Reboots are budgeted. Escalation cannot tell a lying link from a broker that
// is genuinely down; without a budget a broker outage would reboot the node
// every few minutes for the outage's whole duration. After kMaxRecoveryReboots
// consecutive unsuccessful reboots the policy stops rebooting and falls back to
// an occasional radio cycle, which keeps the node sampling and its screen
// alive. The reboot count must live in RTC memory (survives esp_restart) and be
// cleared by the caller once the broker connects.
//
// Pure header (no Arduino includes) so the native test env exercises this exact
// code -- see power_pure.h for why hand-copied test logic is not acceptable.

#include <cstdint>

// Consecutive link checks with an "up" link but no broker before each remedy.
// At the caller's 30 s cadence: radio cycle at 2 min, reboot at 5 min.
#ifndef LINK_RADIO_RESET_CHECKS
#define LINK_RADIO_RESET_CHECKS 4
#endif
#ifndef LINK_REBOOT_CHECKS
#define LINK_REBOOT_CHECKS 10
#endif
// Consecutive recovery reboots allowed before concluding the fault is not ours.
#ifndef LINK_MAX_RECOVERY_REBOOTS
#define LINK_MAX_RECOVERY_REBOOTS 3
#endif

enum class LinkAction : uint8_t {
  kNone = 0,
  kRadioCycle,  // disconnect, radio off/on, re-associate
  kReboot,      // esp_restart(); caller increments the RTC reboot count first
};

struct LinkRecovery {
  uint32_t unreachable_checks = 0;

  // One link-check tick. `reboot_budget_spent` is the RTC-backed count of
  // recovery reboots since the broker was last reached.
  LinkAction on_check(bool wifi_connected, bool mqtt_connected, uint32_t reboot_budget_spent) {
    if (mqtt_connected) {
      // Healthy. The caller clears the RTC reboot count on this edge.
      unreachable_checks = 0;
      return LinkAction::kNone;
    }
    // WiFi-down checks COUNT toward escalation rather than freezing it. The
    // first cut froze here on the theory that the caller's reconnect branch
    // owns a down link -- and then a build with empty credentials baked in
    // (2026-08-01) showed the hole: an image that can never associate never
    // reboots, so boot-health never accumulates the unhealthy boots it needs
    // to roll back, and the node sits dark forever with a good image one slot
    // away. Reboots are budgeted and a confirmed image never rolls back, so
    // counting a down link costs a transient AP outage at most three reboots.
    (void)wifi_connected;

    unreachable_checks++;

    if (reboot_budget_spent >= LINK_MAX_RECOVERY_REBOOTS) {
      // Out of reboots: the broker is probably just down. Keep nudging the
      // radio at the same cadence the first remedy used, forever, so a node
      // whose link DID wedge during the outage still recovers eventually.
      return (unreachable_checks % LINK_RADIO_RESET_CHECKS == 0) ? LinkAction::kRadioCycle
                                                                 : LinkAction::kNone;
    }

    if (unreachable_checks == LINK_RADIO_RESET_CHECKS)
      return LinkAction::kRadioCycle;
    if (unreachable_checks >= LINK_REBOOT_CHECKS)
      return LinkAction::kReboot;
    return LinkAction::kNone;
  }
};
