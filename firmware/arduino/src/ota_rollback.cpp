// App-level OTA rollback implementation. See ota_rollback.h / boot_health.h.
// Copyright 2026 Justin

#include "ota_rollback.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_ota_ops.h"

#include "boot_health.h"

// Consecutive boots that never reached the broker. RTC: survives esp_restart
// (the link-recovery reboots that usually drive this count), not power loss.
RTC_DATA_ATTR static uint32_t rtc_unhealthy_boots = 0;
// Which image owns the streak. An OTA reboots via esp_restart, so without this
// a NEW image would inherit the old image's failure streak and could hit the
// rollback threshold before its first fair chance to confirm.
RTC_DATA_ATTR static char rtc_streak_sha[17] = {0};

static bool g_confirmed_this_boot = false;
static bool g_running_confirmed = false;

// First 16 hex chars of the running image's ELF SHA-256 -- image identity that
// distinguishes "this exact build" from "whatever else has occupied this slot".
static void running_sha_hex(char* out, size_t n) {
  const esp_app_desc_t* desc = esp_app_get_description();
  static const char* kHex = "0123456789abcdef";
  size_t chars = (n - 1 < 16) ? n - 1 : 16;
  for (size_t i = 0; i < chars / 2; i++) {
    out[i * 2] = kHex[desc->app_elf_sha256[i] >> 4];
    out[i * 2 + 1] = kHex[desc->app_elf_sha256[i] & 0xF];
  }
  out[chars] = '\0';
}

static bool image_is_confirmed() {
  char sha[17];
  running_sha_hex(sha, sizeof(sha));
  Preferences prefs;
  if (!prefs.begin("boothealth", true))
    return false;  // namespace does not exist yet: nothing ever confirmed
  String stored = prefs.getString("conf_sha", "");
  prefs.end();
  return stored.length() > 0 && stored.equals(sha);
}

void ota_rollback_check_at_boot() {
  g_running_confirmed = image_is_confirmed();

  // A different image than the one that accumulated the streak starts fresh.
  char sha[17];
  running_sha_hex(sha, sizeof(sha));
  if (strncmp(rtc_streak_sha, sha, sizeof(sha)) != 0) {
    rtc_unhealthy_boots = 0;
    snprintf(rtc_streak_sha, sizeof(rtc_streak_sha), "%s", sha);
  }

  rtc_unhealthy_boots++;  // provisionally unhealthy; mark_healthy() clears it

  Serial.printf("[ROLLBACK] boot %u unhealthy in a row, image %s\n", rtc_unhealthy_boots,
                g_running_confirmed ? "confirmed" : "UNCONFIRMED");

  if (!boot_health_should_rollback(rtc_unhealthy_boots, g_running_confirmed))
    return;

  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  if (!running || !other || other == running) {
    Serial.println("[ROLLBACK] no alternate slot; staying on this image");
    return;
  }

  // set_boot_partition validates the target image; an empty or torn slot fails
  // here and we stay put -- a broken image beats no image.
  esp_err_t err = esp_ota_set_boot_partition(other);
  if (err != ESP_OK) {
    Serial.printf("[ROLLBACK] alternate slot rejected (err=%d); staying\n", err);
    return;
  }

  Serial.printf(
      "[ROLLBACK] %u boots without reaching the broker and this image was "
      "never confirmed - rolling back to %s\n",
      rtc_unhealthy_boots, other->label);
  Serial.flush();
  rtc_unhealthy_boots = 0;
  esp_restart();
}

void ota_rollback_mark_healthy() {
  rtc_unhealthy_boots = 0;
  if (g_confirmed_this_boot)
    return;
  g_confirmed_this_boot = true;
  if (g_running_confirmed)
    return;  // already the stored image; no NVS write needed

  char sha[17];
  running_sha_hex(sha, sizeof(sha));
  Preferences prefs;
  if (prefs.begin("boothealth", false)) {
    prefs.putString("conf_sha", sha);
    prefs.end();
    g_running_confirmed = true;
    Serial.printf("[ROLLBACK] image %s confirmed healthy\n", sha);
  }
}

uint32_t ota_rollback_unhealthy_boots() { return rtc_unhealthy_boots; }
bool ota_rollback_image_confirmed() { return g_running_confirmed; }
