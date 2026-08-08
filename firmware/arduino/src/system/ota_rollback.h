#pragma once
// Copyright 2026 Justin
//
// App-level OTA rollback. Decision logic lives in boot_health.h (pure, natively
// tested); this module owns the hardware pieces: the RTC unhealthy-boot
// counter, the NVS record of the last confirmed image, and the slot flip.
//
// Call ota_rollback_check_at_boot() early in setup, after NVS is available and
// before the network comes up. If it decides to roll back it does not return.
// Call ota_rollback_mark_healthy() once the broker connects; the first call for
// a given image persists it as confirmed.

#include <cstdint>

void ota_rollback_check_at_boot();
void ota_rollback_mark_healthy();
// Diagnostics for the serial console / debug commands.
uint32_t ota_rollback_unhealthy_boots();
bool ota_rollback_image_confirmed();
