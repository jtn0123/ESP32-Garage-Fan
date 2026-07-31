// USB mass-storage implementation. See usb_msc.h for the access model.
// Copyright 2026 Justin

#include "usb_msc.h"

#if FEATURE_USB_MSC

#include <Arduino.h>
#include <USB.h>
#include <USBMSC.h>

#include "esp_partition.h"
#include "wear_levelling.h"

#include "logging.h"
#include "sd_store.h"

// Global on purpose: the constructor registers the MSC interface with tinyusb,
// and with ARDUINO_USB_CDC_ON_BOOT=1 the core starts USB inside app_main()
// BEFORE setup() runs. Static-init is the only window where the interface can
// still be added (verified against cores/esp32/USBMSC.cpp:188 and main.cpp).
static USBMSC g_msc;

static wl_handle_t g_wl = WL_INVALID_HANDLE;
static const esp_partition_t* g_part = nullptr;
static volatile bool g_host_active = false;
static volatile uint32_t g_last_io_ms = 0;

// Take exclusive ownership of the partition for the host.
static bool msc_take_ownership() {
  if (g_wl != WL_INVALID_HANDLE)
    return true;
  // FFat (and the whole storage layer) must let go first: two wear-levelling
  // instances on one partition corrupt each other's sector mapping.
  sd_end();
  if (wl_mount(g_part, &g_wl) != ESP_OK) {
    g_wl = WL_INVALID_HANDLE;
    LOG_WARN("USB-MSC: wl_mount failed; remounting storage");
    sd_begin();
    return false;
  }
  g_host_active = true;
  LOG_INFO("USB-MSC: host took the disk (storage writes paused)");
  return true;
}

// Give the partition back to the firmware.
static void msc_release_ownership(const char* why) {
  if (g_wl != WL_INVALID_HANDLE) {
    wl_unmount(g_wl);
    g_wl = WL_INVALID_HANDLE;
  }
  if (g_host_active) {
    g_host_active = false;
    LOG_INFO("USB-MSC: released (%s); storage remounting", why);
  }
  sd_begin();
}

static int32_t msc_on_read(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  if (!msc_take_ownership())
    return -1;
  g_last_io_ms = millis();
  const size_t addr = static_cast<size_t>(lba) * 512 + offset;
  if (wl_read(g_wl, addr, buffer, bufsize) != ESP_OK)
    return -1;
  return static_cast<int32_t>(bufsize);
}

static int32_t msc_on_write(uint32_t, uint32_t, uint8_t*, uint32_t) {
  // Read-only medium: refuse every write. Hosts see a write-protected disk.
  return -1;
}

static bool msc_on_start_stop(uint8_t, bool start, bool load_eject) {
  if (!start && load_eject) {
    // Host ejected the disk: hand storage back immediately.
    msc_release_ownership("host eject");
  }
  return true;
}

void usb_msc_begin() {
  g_part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat");
  if (!g_part) {
    LOG_WARN("USB-MSC: no ffat partition; feature disabled");
    return;
  }

  // Geometry: the FAT filesystem lives in the wear-levelling LOGICAL space,
  // which is the partition minus WL bookkeeping. Mount briefly to measure it,
  // then release so the storage layer keeps ownership until a host shows up.
  wl_handle_t probe = WL_INVALID_HANDLE;
  if (wl_mount(g_part, &probe) != ESP_OK) {
    LOG_WARN("USB-MSC: wl probe failed; feature disabled");
    return;
  }
  const size_t logical_bytes = wl_size(probe);
  wl_unmount(probe);

  g_msc.vendorID("ESP32");
  g_msc.productID("TempSensorData");
  g_msc.productRevision("1.0");
  g_msc.onRead(msc_on_read);
  g_msc.onWrite(msc_on_write);
  g_msc.onStartStop(msc_on_start_stop);
  g_msc.mediaPresent(true);
  g_msc.begin(logical_bytes / 512, 512);
  LOG_INFO("USB-MSC: exposing %u KB read-only", static_cast<unsigned>(logical_bytes / 1024));
}

void usb_msc_tick() {
  // Safety net for a host that unplugs without ejecting: after 60 s with no
  // sector reads, reclaim the disk so history logging resumes. A still-attached
  // idle host just re-triggers the handoff on its next read.
  if (g_host_active && (millis() - g_last_io_ms) > 60000) {
    msc_release_ownership("io idle timeout");
  }
}

bool usb_msc_host_active() { return g_host_active; }

#endif  // FEATURE_USB_MSC
