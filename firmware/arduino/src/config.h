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
// Which 2.13" FeatherWing is fitted. Both parts are SSD1680 at 250x122 and take
// the identical driver, so this changes NO drawing -- it only tells the web
// console whether the red plane actually appears on the glass, so its mirror
// shows what this device displays rather than an idealised version.
//
// The fitted glass showed the header rule in red (2026-08-12), which settles
// it: this is the tricolor part, whatever docs/HARDWARE.md guessed. Everything
// drawn red is an accent over information already legible in black (see the
// TRICOLOR RULE in ui/display_layout.h), so flipping this is safe in either
// direction: the panel keeps working, the mirror just stops lying about the
// colour.
#ifndef FAN_EPD_TRICOLOR
#define FAN_EPD_TRICOLOR 1
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

// POSIX TZ string for the site. SNTP is configured with configTime(0, 0, ...),
// i.e. the system clock is UTC -- correct, and what every logged epoch should
// stay in. This is only for the places that must reason about the OPERATOR's
// day.
//
// It exists because odometer's "fan today" counter derived its day key with
// gmtime_r, so run_today_s reset at UTC midnight: 17:00 PDT, in the middle of
// the hottest hours the fan actually runs. Override FAN_TZ in .env for a site
// in another zone; the default matches the deployed unit.
#ifndef FAN_TZ
#define FAN_TZ "PST8PDT,M3.2.0,M11.1.0"
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

// ------------------------------------------------------- plug watt meter
// Home Assistant entity ids for the Tapo P110M on the fan's supply (see
// net/plug.h). Overridable for a different plug; HA_URL/HA_TOKEN come from
// .env via generated_config.h.
#ifndef FAN_PLUG_POWER_ENTITY
#define FAN_PLUG_POWER_ENTITY "sensor.garage_garage_fan_tapo_p110m_plug_power"
#endif
#ifndef FAN_PLUG_VOLT_ENTITY
#define FAN_PLUG_VOLT_ENTITY "sensor.garage_garage_fan_tapo_p110m_plug_effective_voltage"
#endif

// ---------------------------------------------------------------- MQTT
inline constexpr const char* kTopicSet = "garage/fan/set";
inline constexpr const char* kTopicState = "garage/fan/state";
inline constexpr const char* kTopicAvail = "garage/fan/availability";
inline constexpr const char* kTopicClimate = "garage/climate";
// Retained standing alert: {"kind":"plug_disagree",...} while the watt meter
// contradicts the commanded speed, {"kind":"ok"} otherwise. Retained so a HA
// automation sees the state on subscribe, not only on the edge; republished
// on every broker connect so a reboot cannot leave a stale alarm standing.
inline constexpr const char* kTopicAlert = "garage/fan/alert";
// There is no subscribed outdoor topic any more: the fan polls open-meteo
// itself (net/weather) and sensors/outdoor.h explains why the second feed
// had to go rather than be preferred.

// ------------------------------------------------------------- cadences
inline constexpr uint32_t kUnconfirmedDeadlineMs = 5 * 60 * 1000;
inline constexpr uint32_t kSampleMs = 5 * 60 * 1000;  // ring + SD cadence
inline constexpr uint32_t kAutoTickMs = 30 * 1000;
// Three missed 10-minute weather polls. Past this the reading expires and
// fan_auto_decide sees NAN, which it reads as "hold speed and latch".
inline constexpr uint32_t kOutdoorStaleMs = 30 * 60 * 1000;
inline constexpr uint32_t kMqttGraceMs = 10 * 1000;  // retained-replay window

// --------------------------------------------------------------- history
inline constexpr uint16_t kRingLen = 288;  // 24 h at 5 min -- the console's
                                           // "LAST 24 HOURS" label depends on
                                           // this matching kSampleMs
inline constexpr uint16_t kGraphMaxPts = 288;
