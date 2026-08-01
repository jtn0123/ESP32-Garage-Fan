#pragma once
// Copyright 2026 Justin
//
// Boot-health rollback decision, as pure logic.
//
// The gap this closes: the partition table has two app slots (ota_0/ota_1) but
// nothing ever exercised the bootloader's rollback machinery, so an OTA image
// that boots yet never reaches the network is permanent until someone gets a
// cable to the device -- which the bad image itself makes impossible remotely.
// Observed for real on 2026-08-01.
//
// App-level scheme, no bootloader config needed:
//  - an RTC counter tracks consecutive boots that never reached the broker
//    (RTC survives esp_restart -- including the link-recovery reboots that are
//    usually what is driving the count -- but not power loss, so a power cycle
//    innocently restarts the count);
//  - NVS records the ELF SHA of the last image that EVER reached the broker
//    ("confirmed"). NVS survives power loss.
//
// Decision at boot: roll back only when the running image has never been
// confirmed AND the unhealthy streak has hit the threshold. A confirmed image
// never rolls back -- if a once-working image cannot reach the broker, the
// broker is the problem, and flipping slots would just regress the firmware
// version without fixing anything.
//
// Pure header (no Arduino includes) so the native test env compiles this exact
// logic -- see power_pure.h for why hand-copied test logic is not acceptable.

#include <cstdint>

// Consecutive unhealthy boots of an unconfirmed image before flipping slots.
// The link-recovery policy reboots at ~5 min intervals and budgets 3 reboots,
// so a networkless OTA image reaches this threshold on its own in ~10-15 min --
// and the check runs at boot START, so the flip happens on the budget's last
// boot rather than needing a fourth that the budget would never grant.
#ifndef BOOT_HEALTH_MAX_UNHEALTHY
#define BOOT_HEALTH_MAX_UNHEALTHY 3
#endif

// `unhealthy_boots` INCLUDES the boot being decided (caller increments first).
// `running_image_confirmed`: the running image's SHA matches the stored
// last-confirmed SHA.
inline bool boot_health_should_rollback(uint32_t unhealthy_boots, bool running_image_confirmed) {
  if (running_image_confirmed)
    return false;
  return unhealthy_boots >= BOOT_HEALTH_MAX_UNHEALTHY;
}
