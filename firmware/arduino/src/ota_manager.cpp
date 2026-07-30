// OTA update implementation
// Copyright 2024 Justin

#include "ota_manager.h"

#if FEATURE_OTA

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "logging.h"
#include "runtime_config.h"
#include "sd_store.h"

namespace {

bool g_active = false;
bool g_in_progress = false;
uint8_t g_last_logged_pct = 0;
char g_sd_result[64] = "not attempted";

void set_sd_result(const char* msg) {
  strncpy(g_sd_result, msg, sizeof(g_sd_result) - 1);
  g_sd_result[sizeof(g_sd_result) - 1] = '\0';
}

// An ESP32 application image always starts with this magic byte. Checking it up
// front turns "garbage file on the card" into a clear log line instead of a
// half-written flash slot.
constexpr uint8_t kEspImageMagic = 0xE9;

}  // namespace

void ota_begin(const char* hostname) {
  if (g_active)
    return;

  if (!rc_ota_enabled()) {
    LOG_INFO("OTA: disabled by config");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    LOG_WARN("OTA: WiFi down, network OTA not started");
    return;
  }

  const char* password = rc_ota_password();
  const bool has_password = password && password[0] != '\0';

  if (!has_password) {
#if OTA_REQUIRE_PASSWORD
    LOG_ERROR("OTA: no password set and OTA_REQUIRE_PASSWORD=1 - refusing to start");
    return;
#else
    // Anyone on the same network can push firmware to an unauthenticated
    // listener. Loud on purpose.
    LOG_WARN("OTA: *** no password set - any host on this LAN can reflash this device ***");
    LOG_WARN("OTA: set ota.password in /config/device.json on the SD card to fix");
#endif
  }

  if (hostname && hostname[0]) {
    ArduinoOTA.setHostname(hostname);
  }
  ArduinoOTA.setPort(OTA_PORT);
  if (has_password) {
    ArduinoOTA.setPassword(password);
  }

  // mDNS is owned by app_controller (it also advertises the espsensor service).
  // Letting ArduinoOTA re-run MDNS.begin() here would discard that record, so
  // advertise the Arduino OTA service explicitly instead.
  ArduinoOTA.setMdnsEnabled(false);
  MDNS.enableArduino(OTA_PORT, has_password);

  ArduinoOTA.onStart([]() {
    g_in_progress = true;
    g_last_logged_pct = 0;
    const char* what = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
    LOG_INFO("OTA: update starting (%s)", what);
    // Close the card cleanly so a half-written log or CSV is not left behind
    // when the device reboots into the new image.
    sd_end();
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    // The transfer runs inside ArduinoOTA.handle() for far longer than the
    // watchdog timeout, so it has to be fed from here.
    esp_task_wdt_reset();

    if (total == 0)
      return;
    uint8_t pct = static_cast<uint8_t>((progress * 100ULL) / total);
    if (pct >= g_last_logged_pct + 10) {
      g_last_logged_pct = pct;
      LOG_INFO("OTA: %u%% (%u/%u bytes)", pct, progress, total);
    }
  });

  ArduinoOTA.onEnd([]() {
    g_in_progress = false;
    LOG_INFO("OTA: update complete, rebooting");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    g_in_progress = false;
    const char* reason = "unknown";
    switch (error) {
      case OTA_AUTH_ERROR:
        reason = "auth failed";
        break;
      case OTA_BEGIN_ERROR:
        reason = "begin failed (image too large?)";
        break;
      case OTA_CONNECT_ERROR:
        reason = "connect failed";
        break;
      case OTA_RECEIVE_ERROR:
        reason = "receive failed";
        break;
      case OTA_END_ERROR:
        reason = "end failed (bad image)";
        break;
      default:
        break;
    }
    LOG_ERROR("OTA: error %u - %s", static_cast<unsigned>(error), reason);
    // The running firmware is untouched by a failed update; remount so logging
    // and history keep working.
    sd_begin();
  });

  ArduinoOTA.begin();
  g_active = true;
  LOG_INFO("OTA: listening on port %d as '%s' (%s)", OTA_PORT, hostname ? hostname : "(default)",
           has_password ? "password set" : "NO PASSWORD");
#if !ALWAYS_ON
  // In the deep-sleep build the device is only awake for a few seconds per
  // interval, so an upload has almost no chance of landing. Say so rather than
  // leaving a listener that looks usable.
  LOG_WARN("OTA: deep-sleep build - only reachable during diagnostic mode.");
  LOG_WARN("OTA: build with -DALWAYS_ON=1, or update from the SD card instead.");
#endif
}

void ota_loop() {
  if (!g_active)
    return;
  ArduinoOTA.handle();
}

bool ota_in_progress() { return g_in_progress; }

bool ota_is_active() { return g_active; }

bool ota_apply_from_sd() {
  if (!sd_has_staged_update()) {
    set_sd_result("no staged image");
    return false;
  }

  size_t size = 0;
  File image = sd_open_staged_update(&size);
  if (!image) {
    set_sd_result("could not open image");
    LOG_ERROR("OTA/SD: could not open %s", SD_UPDATE_PATH);
    return false;
  }

  LOG_INFO("OTA/SD: found %s (%u bytes)", SD_UPDATE_PATH, static_cast<unsigned>(size));

  if (size == 0) {
    image.close();
    set_sd_result("image is empty");
    LOG_ERROR("OTA/SD: image is empty");
    sd_finish_staged_update(false);
    return false;
  }

  int magic = image.peek();
  if (magic != kEspImageMagic) {
    image.close();
    set_sd_result("bad magic byte, not an ESP32 image");
    LOG_ERROR("OTA/SD: first byte is 0x%02X, expected 0x%02X - not a firmware image", magic,
              kEspImageMagic);
    sd_finish_staged_update(false);
    return false;
  }

  if (!Update.begin(size, U_FLASH)) {
    image.close();
    set_sd_result("Update.begin rejected the image");
    LOG_ERROR("OTA/SD: Update.begin failed: %s", Update.errorString());
    sd_finish_staged_update(false);
    return false;
  }

  g_in_progress = true;
  uint8_t buf[1024];
  size_t written = 0;
  uint8_t last_pct = 0;
  bool io_error = false;

  while (written < size) {
    int n = image.read(buf, sizeof(buf));
    if (n <= 0) {
      io_error = true;
      break;
    }
    if (Update.write(buf, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
      io_error = true;
      break;
    }
    written += static_cast<size_t>(n);

    esp_task_wdt_reset();

    uint8_t pct = static_cast<uint8_t>((written * 100ULL) / size);
    if (pct >= last_pct + 10) {
      last_pct = pct;
      LOG_INFO("OTA/SD: %u%% (%u/%u bytes)", pct, static_cast<unsigned>(written),
               static_cast<unsigned>(size));
    }
  }

  image.close();

  if (io_error || written != size) {
    Update.abort();
    g_in_progress = false;
    set_sd_result("short read or flash write error");
    LOG_ERROR("OTA/SD: aborted after %u of %u bytes: %s", static_cast<unsigned>(written),
              static_cast<unsigned>(size), Update.errorString());
    sd_finish_staged_update(false);
    return false;
  }

  if (!Update.end(true)) {
    g_in_progress = false;
    set_sd_result("image failed verification");
    LOG_ERROR("OTA/SD: Update.end failed: %s", Update.errorString());
    sd_finish_staged_update(false);
    return false;
  }

  g_in_progress = false;
  set_sd_result("applied, rebooting");
  LOG_INFO("OTA/SD: image applied successfully, rebooting into it");

  // Mark it before rebooting, otherwise the same image is applied on every boot.
  sd_finish_staged_update(true);
  sd_end();

  delay(200);  // let the serial log drain
  ESP.restart();
  return true;  // not reached
}

const char* ota_last_sd_result() { return g_sd_result; }

#else  // !FEATURE_OTA

void ota_begin(const char*) {}
void ota_loop() {}
bool ota_in_progress() { return false; }
bool ota_is_active() { return false; }
bool ota_apply_from_sd() { return false; }
const char* ota_last_sd_result() { return "OTA compiled out"; }

#endif  // FEATURE_OTA
