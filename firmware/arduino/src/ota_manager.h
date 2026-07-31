#pragma once
// Copyright 2024 Justin
//
// Over-the-air firmware updates, by two routes:
//
//  1. Network (ArduinoOTA / espota on OTA_PORT). Requires the device to be
//     reachable, which in practice means ALWAYS_ON — a deep-sleeping node is
//     awake too briefly to catch an upload.
//  2. From the microSD card: an image left at /firmware/update.bin is verified
//     and applied during boot. This works regardless of sleep mode and needs no
//     network, at the cost of physically moving the card.
//
// Both write to the inactive OTA slot and let the bootloader swap on reboot, so
// a failed or interrupted update leaves the running firmware intact. The board's
// stock partition table already provides two 1408 KB app slots.

#include <Arduino.h>
#include "feature_flags.h"

// Set up the network OTA listener. Safe to call when WiFi is down (it simply
// does nothing) and safe to call when OTA is disabled in config.
void ota_begin(const char* hostname);

// Service the OTA listener. Must be called frequently from loop() — an upload
// only progresses while this is running.
void ota_loop();

// True while an update is being received, so callers can avoid starting a
// display refresh or anything else that would stall the transfer.
bool ota_in_progress();

bool ota_is_active();

// Apply /firmware/update.bin if present. Call during setup, after storage_begin().
// Reboots into the new image on success and does not return. Returns false when
// there is nothing to apply or the image was rejected.
bool ota_apply_from_sd();

// Human-readable result of the last SD update attempt, for logging/MQTT.
const char* ota_last_sd_result();
