#pragma once
// Copyright 2024 Justin

// Pulled in first so the #ifndef fallbacks below act as fallbacks rather than
// shadowing values generated from config/device.yaml. Precedence ends up:
// -D build flag > config/device.yaml (generated_config.h) > default here.
#include "generated_config.h"

// Compile-time toggles (filled in later as features land)
#ifndef USE_DISPLAY
#define USE_DISPLAY 1
#endif
#ifndef DEV_NO_SLEEP
#define DEV_NO_SLEEP 0
#endif
#ifndef DEV_CYCLE_MODE
#define DEV_CYCLE_MODE 0
#endif
#ifndef DEV_ACTIVE_SEC
#define DEV_ACTIVE_SEC 180
#endif
#ifndef DEV_SLEEP_SEC
#define DEV_SLEEP_SEC 180
#endif
// Always-on mode: instead of waking, publishing and deep sleeping, the device
// stays powered and samples on a timer. This is what makes network OTA usable —
// a deep-sleeping node is unreachable for all but a few seconds per interval.
// Costs roughly 50x the average current of the deep-sleep cycle, so it suits
// USB-powered nodes.
#ifndef ALWAYS_ON
#define ALWAYS_ON 0
#endif
// How often to read the sensors and publish while in ALWAYS_ON mode.
// Kept at/above 60s: the BME280 self-heats when polled continuously, which
// biases the temperature reading upward.
#ifndef SAMPLE_INTERVAL_SEC
#define SAMPLE_INTERVAL_SEC 300
#endif
// Floor on how often the e-ink panel is redrawn in ALWAYS_ON mode. Sampling
// every 5 minutes does not mean redrawing every 5 minutes: a full refresh
// visibly flashes the panel and consumes one of its finite refresh cycles, so
// the draw is decoupled from the sample and rate limited here.
#ifndef DISPLAY_MIN_REFRESH_INTERVAL_SEC
#define DISPLAY_MIN_REFRESH_INTERVAL_SEC 900
#endif
#ifndef USE_SHT40
#define USE_SHT40 0
#endif
#ifndef USE_BME280
#define USE_BME280 1
#endif
#define USE_MQTT 1
#define USE_ESPHOME_API 0
// Support either fuel gauge; runtime probe decides which one is present
#ifndef USE_MAX17048
#define USE_MAX17048 1
#endif
#ifndef USE_LC709203F
#define USE_LC709203F 1
#endif

// Wi-Fi provisioning (ESP-IDF wifi_prov_mgr) compile-time switch
#ifndef USE_WIFI_PROVISIONING
#define USE_WIFI_PROVISIONING 0
#endif
// Select provisioning transport. ESP32-S2 has no BLE, so SoftAP by default.
#ifndef WIFI_PROV_USE_BLE
#define WIFI_PROV_USE_BLE 0
#endif
#ifndef WIFI_PROV_USE_SOFTAP
#define WIFI_PROV_USE_SOFTAP 1
#endif
// Security level: 1 enables proof-of-possession. 0 disables POP (simpler).
#ifndef WIFI_PROV_SECURITY
#define WIFI_PROV_SECURITY 0
#endif
// Timeout for provisioning session before giving up (seconds)
#ifndef WIFI_PROV_TIMEOUT_SEC
#define WIFI_PROV_TIMEOUT_SEC 180
#endif

#ifndef I2C_TIMEOUT_MS
#define I2C_TIMEOUT_MS 150
#endif
// Default I2C bus speed
#ifndef I2C_CLOCK_HZ
#define I2C_CLOCK_HZ 100000
#endif
// Optional: scan I2C bus and print detected addresses at startup
#ifndef I2C_DEBUG_SCAN
#define I2C_DEBUG_SCAN 0
#endif

// Phase budgets (ms). Keep each phase bounded so a stall can't eat the whole
// wake.
#ifndef SENSOR_PHASE_TIMEOUT_MS
#define SENSOR_PHASE_TIMEOUT_MS 300
#endif
#ifndef FETCH_RETAINED_TIMEOUT_MS
#define FETCH_RETAINED_TIMEOUT_MS 2000
#endif
#ifndef DISPLAY_PHASE_TIMEOUT_MS
#define DISPLAY_PHASE_TIMEOUT_MS 8000
#endif
#ifndef PUBLISH_PHASE_TIMEOUT_MS
#define PUBLISH_PHASE_TIMEOUT_MS 800
#endif
// Debounce window to coalesce multiple MQTT outside updates before a draw
#ifndef MQTT_OUTSIDE_DEBOUNCE_MS
#define MQTT_OUTSIDE_DEBOUNCE_MS 1200
#endif

// Logging system configuration
#ifndef LOG_ENABLED
#define LOG_ENABLED 1
#endif

#ifndef LOG_MQTT_ENABLED
#define LOG_MQTT_ENABLED 1
#endif

// Status pixel configuration
#ifndef USE_STATUS_PIXEL
#define USE_STATUS_PIXEL 0
#endif
// The Feather variant names the on-board NeoPixel PIN_NEOPIXEL; the firmware
// historically guarded on NEOPIXEL_PIN, which nothing defines — so every pixel
// code path was silently compiled out and the board showed no boot feedback at
// all. Bridge the name here, where every TU that cares already includes this.
#if USE_STATUS_PIXEL && defined(PIN_NEOPIXEL) && !defined(NEOPIXEL_PIN)
#define NEOPIXEL_PIN PIN_NEOPIXEL
#endif

// microSD card on the eInk FeatherWing.
// The card shares the Feather's hardware SPI bus with the e-ink panel; only the
// chip selects differ (panel = D9, card = D5). See hardware/pinmap.md.
#ifndef SD_CS_PIN
#define SD_CS_PIN 5
#endif
// The Wing also carries a 23K SRAM chip on the same bus (CS = D6). Nothing here
// uses it, but it is the only other device that drives MISO, and GxEPD2 -- unlike
// Adafruit_EPD -- never touches that pin. Left floating it can answer alongside
// the card, so sd_begin() parks it high before mounting. See hardware/pinmap.md.
#ifndef SRAM_CS_PIN
#define SRAM_CS_PIN 6
#endif
// SPI clock for the card. 20 MHz is within microSD spec on paper, but this bus is
// shared with the panel and routed through stacking headers, and at 20 MHz the
// card would not enumerate on real hardware. 4 MHz is the SD library's own
// default and mounts reliably here; the card is written a few times per sample,
// so the throughput difference is not worth the margin.
#ifndef SD_SPI_FREQ_HZ
#define SD_SPI_FREQ_HZ 4000000
#endif
// Retention sweep for /data/*.csv. 0 disables pruning (keep everything).
#ifndef SD_HISTORY_RETENTION_DAYS
#define SD_HISTORY_RETENTION_DAYS 90
#endif
// Number of rotating log files kept in /logs.
#ifndef SD_LOG_FILE_COUNT
#define SD_LOG_FILE_COUNT 5
#endif
// Size at which the active log file rotates (bytes).
#ifndef SD_LOG_MAX_BYTES
#define SD_LOG_MAX_BYTES 262144
#endif

// Added to the raw inside-temperature reading before it is published, displayed
// or stored. The BME280 sits close enough to the ESP32 to pick up its self-heat,
// so an always-on node reads high; measure the steady-state delta against a
// trusted thermometer and set this negative to correct it (e.g. -6.0f).
// Runtime-overridable as "temp_offset_c" in /config/device.json.
#ifndef TEMP_OFFSET_C
#define TEMP_OFFSET_C 0.0f
#endif

// LC709203F fuel-gauge APA (Adjustment Pack Application) register value.
// Default matches the library's 3000 mAh constant; override per hardware. The
// datasheet table runs ~9 counts per 1000 mAh above 1000 mAh (0x19=1000,
// 0x2D=2000, 0x36=3000), so larger custom packs extrapolate linearly.
#ifndef BATTERY_APA
#define BATTERY_APA 0x36
#endif

// POSIX TZ string for the header clock and every timestamp (history CSV, logs).
// Overridable via `timezone:` in config/device.yaml. Default is US Pacific —
// where the deployed unit lives; the previous hardcoded EST5EDT rendered the
// header clock three hours ahead.
#ifndef TIME_TZ
#define TIME_TZ "PST8PDT,M3.2.0,M11.1.0"
#endif

// Average current draw of the always-on build after the heat mitigations
// (80 MHz, modem sleep, 11 dBm TX), used for the footer's battery-days
// estimate. Refine from the observed discharge slope rather than trusting it
// to two digits.
#ifndef ALWAYS_ON_AVG_CURRENT_MA
#define ALWAYS_ON_AVG_CURRENT_MA 35.0f
#endif

// Network OTA (ArduinoOTA / espota) listener port.
#ifndef OTA_PORT
#define OTA_PORT 3232
#endif
// Refuse to start the network OTA listener unless a password is configured.
//
// Defaults to OFF, which is the repository owner's deliberate choice for a
// private, trusted home network: OTA then works with no setup step. Understand
// what it means before copying this elsewhere — an unauthenticated listener lets
// any host on the LAN replace this device's firmware, and an always-on node
// advertises itself over mDNS. The firmware logs a prominent warning at every
// boot while running this way.
//
// Set ota.password in /config/device.json (or -DOTA_PASSWORD=...) to
// authenticate, and -DOTA_REQUIRE_PASSWORD=1 to make an unset password a hard
// failure rather than a warning. Updating from the SD card is unaffected either
// way; it never uses a password.
#ifndef OTA_REQUIRE_PASSWORD
#define OTA_REQUIRE_PASSWORD 0
#endif
