#pragma once
// Every pin, topic, tunable and identity string for the fan controller.
//
// Each pin and feature knob is #ifndef-guarded so a -D build flag wins without
// editing this file. The k* constants are constexpr rather than macros so they
// carry types into the modules that use them.
//
// Hardware ground truth is docs/fan_protocol/PROTOCOL.md; the duty table below
// is measured, not derived. Do not "tidy" it.

#include <stdint.h>

#include "generated_config.h"  // WIFI_*, MQTT_*, FW_VERSION -- gitignored, generated

// ---------------------------------------------------------------- pins
#ifndef FAN_PWM_PIN
#define FAN_PWM_PIN 18  // A0 -> shifter LV1 -> HV1 -> fan D+
#endif
#ifndef FAN_SPARE_PIN
#define FAN_SPARE_PIN 17  // A1 -> shifter LV2; idles HIGH like the real D-
#endif
// Shared SPI bus with the e-ink FeatherWing: park every select high before
// touching the SD card.
#ifndef SD_CS_PIN
#define SD_CS_PIN 5
#endif
#ifndef SRAM_CS_PIN
#define SRAM_CS_PIN 6
#endif
#ifndef EPD_CS_PIN
#define EPD_CS_PIN 9
#endif
#ifndef EPD_DC_PIN
#define EPD_DC_PIN 10
#endif

// ------------------------------------------------------------ identity
#ifndef FAN_HOSTNAME
#define FAN_HOSTNAME "garage-fan"
#endif
#ifndef FAN_OTA_TOKEN
#define FAN_OTA_TOKEN "iliving-ota"
#endif
// GitHub project the console's update check queries. The device never talks to
// GitHub itself -- it hands the browser this slug and the browser does the
// lookup. See web/src/update.ts for why the check lives there.
#ifndef FAN_GITHUB_REPO
#define FAN_GITHUB_REPO "jtn0123/ESP32-Garage-Fan"
#endif

// One source of truth: repo-root VERSION -> gen_device_header.py -> FW_VERSION
// -> here -> /api/state -> the console header and deploy.sh's post-flash
// verify. Never hardcode a version string: a tagged release would then ship a
// binary that reports a different number, and the console's update check
// compares exactly this against the newest GitHub tag.
inline constexpr const char* kFwVersion = FW_VERSION;

// ------------------------------------------------------------- protocol
inline constexpr uint16_t kPeriodUs = 9934;
// HIGH width per setting 0..12, mirrored from the wall-controller captures
// after live fan testing showed this rig tracks the HIGH fraction (see
// PROTOCOL.md). Off = solid LOW. /api/raw re-derives empirically if needed.
inline constexpr uint16_t kHighUs[13] = {0,    3477, 4072, 4868, 5066, 5661, 6159,
                                         6754, 7251, 7847, 8344, 8940, 9437};

// ---------------------------------------------------------------- MQTT
inline constexpr const char* kTopicSet = "garage/fan/set";
inline constexpr const char* kTopicState = "garage/fan/state";
inline constexpr const char* kTopicAvail = "garage/fan/availability";
inline constexpr const char* kTopicClimate = "garage/climate";
inline constexpr const char* kTopicOutdoor = MQTT_SUB_BASE "/temp_f";

// ------------------------------------------------------------- cadences
inline constexpr uint32_t kUnconfirmedDeadlineMs = 5 * 60 * 1000;
inline constexpr uint32_t kSampleMs = 5 * 60 * 1000;  // ring + SD cadence
inline constexpr uint32_t kAutoTickMs = 30 * 1000;
inline constexpr uint32_t kOutdoorStaleMs = 30 * 60 * 1000;
inline constexpr uint32_t kMqttGraceMs = 10 * 1000;  // retained-replay window

// --------------------------------------------------------------- history
inline constexpr uint16_t kRingLen = 288;  // 24 h at 5 min -- the console's
                                           // "LAST 24 HOURS" label depends on
                                           // this matching kSampleMs
inline constexpr uint16_t kGraphMaxPts = 288;
