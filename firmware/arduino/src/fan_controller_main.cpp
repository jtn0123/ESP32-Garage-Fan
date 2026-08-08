// Garage fan controller: the ESP32 replaces the iLiving wall controller,
// replaying its PWM protocol (docs/fan_protocol/PROTOCOL.md; polarity
// corrected against the real fan 2026-08-04) through a BSS138 shifter.
//
// Control:  web UI http://garage-fan.local/ · /api/set?speed=0..12 ·
//           MQTT garage/fan/set · /api/raw?high_pct= (calibration)
// Auto:     differential thermostat vs outdoors (fan/auto_logic.h, natively
//           tested). Outdoor temp arrives on MQTT_SUB_BASE "/temp_f" from the
//           existing home/outdoor feed. Hotter outside -> min speed; hotter
//           inside -> ramp toward the user's max. Manual set disables auto.
// Climate:  BME280 samples every 5 min -> 24 h RAM ring + CSV on the microSD
//           card (monthly files, epoch-stamped once SNTP syncs) -> web graph
//           at 24 h / 7 d / 30 d ranges.
// Persist:  speed + auto config in NVS (restored before WiFi), commands also
//           retained on the broker; whichever answers first wins the tie.
// OTA:      POST /update?token=...; A/B slots with ota_rollback confirm.
//
// This file is the orchestrator and nothing else: boot order, the loop
// cadences, and the glue between modules that must not know about each other
// (fan -> transports via the notify hook, climate sample -> ring/SD/MQTT).
// State lives in the modules; resist growing globals back here.
#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>

#include <cmath>

#include "config.h"
#include "esp_task_wdt.h"
#include "fan/control.h"
#include "net/mqtt_link.h"
#include "net/web.h"
#include "sensors/battery.h"
#include "sensors/climate.h"
#include "storage/history.h"
#include "storage/sdcard.h"
#include "system/crashlog.h"
#include "system/odometer.h"
#include "system/ota_rollback.h"
#include "system/timeutil.h"
#include "ui/display.h"

static Preferences g_prefs;
static bool g_services_up = false;
static uint32_t g_last_sample_ms = 0;
static uint32_t g_last_auto_ms = 0;

// One 5-minute sample: read the corrected climate, snapshot everything that
// belongs in the same row (battery first, so the row records the verdict this
// reading produced), then fan it out to the ring, the card and the broker.
static void sample_climate() {
  float t, h, p;
  if (!climate::sample(&t, &h, &p))
    return;
  battery::sample();
  const float oc = climate::outside_c_fresh();
  history::Sample row;
  row.t = t;
  row.h = h;
  row.p = p;
  row.out_f = isnan(oc) ? NAN : oc * 9 / 5 + 32;
  row.speed = (int8_t)(fan::speed() < 0 ? 0 : fan::speed());
  row.batt_v = battery::kind() ? battery::volts() : NAN;
  row.chg = battery::kind() ? (battery::charging() ? 1 : 0) : -1;
  history::append(row, time_synced());
  if (time_synced())
    sdcard::log_sample(time(nullptr), t, h, p, row.out_f, fan::speed());
  mqtt_link::publish_climate(t, h, p);
}

void setup() {
  pinMode(FAN_PWM_PIN, OUTPUT);
  digitalWrite(FAN_PWM_PIN, LOW);  // off on this rig = line low
  pinMode(FAN_SPARE_PIN, OUTPUT);
  digitalWrite(FAN_SPARE_PIN, HIGH);
  Serial.begin(115200);
  ota_rollback_check_at_boot();
  g_prefs.begin("fanctl", false);
  battery::restore(&g_prefs);
  climate::restore(&g_prefs);
  // Announce speed changes over whatever transports are up. Registered before
  // fan::restore so even the boot-time resume publishes once MQTT connects.
  fan::set_notify([](int speed, bool from_mqtt) {
    mqtt_link::publish_state(speed);
    if (!from_mqtt)
      mqtt_link::echo_set(speed);
    web::push_state();
  });
  fan::restore(&g_prefs);
  odometer::restore(&g_prefs);
  // SD stays OUT of the boot path: the controller comes fully online first,
  // and the first mount attempt happens ~60 s later from loop(). A card that
  // crashed a previous boot is quarantined here and never retried.
  sdcard::set_quarantined(crashlog::examine_boot(&g_prefs));
  if (sdcard::quarantined())
    Serial.println("[SD] previous boot died in an SD op -- card quarantined");
  // Task watchdog: a frozen loop (hung bus op, wedged driver) force-reboots
  // in 60 s instead of hanging dark forever. The fan rides through on RMT.
  // 60 s tolerates worst-case framework writes to one stalled peer (10 s per
  // NetworkClient::write call) on the small JSON endpoints while still
  // catching genuine hangs. Large payloads use http_tx and never stall.
  esp_task_wdt_config_t wdt_cfg = {.timeout_ms = 60000, .idle_core_mask = 0, .trigger_panic = true};
  if (esp_task_wdt_init(&wdt_cfg) != ESP_OK)
    esp_task_wdt_reconfigure(&wdt_cfg);
  esp_task_wdt_add(NULL);
  Wire.begin();
  SPI.begin();
  display::begin();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(FAN_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org");
  mqtt_link::init();
  web::begin(&g_prefs);
  Serial.printf("garage fan controller %s: waiting for wifi\n", kFwVersion);
}

void loop() {
  static wl_status_t last_wifi = WL_NO_SHIELD;
  const wl_status_t now = WiFi.status();
  if (now != last_wifi) {
    last_wifi = now;
    if (now == WL_CONNECTED) {
      Serial.printf("wifi up: %s\n", WiFi.localIP().toString().c_str());
      if (!g_services_up) {
        g_services_up = true;
        MDNS.begin(FAN_HOSTNAME);
        MDNS.addService("http", "tcp", 80);
        web::start();
        Serial.printf("web ui: http://%s.local/\n", FAN_HOSTNAME);
      }
    }
  }
  esp_task_wdt_reset();
  mqtt_link::tick();
  web::handle();
  odometer::tick(fan::speed(), fan::watts(fan::speed()));
  display::maybe_render();
  if (g_last_sample_ms == 0 || millis() - g_last_sample_ms >= kSampleMs) {
    g_last_sample_ms = millis();
    sample_climate();
    web::push_state();
  }
  if (millis() - g_last_auto_ms >= kAutoTickMs) {
    g_last_auto_ms = millis();
    fan::tick_auto();
    battery::begin();
    battery::read();
    sdcard::retry_tick();
  }
  if (!mqtt_link::ever_connected() && !ota_rollback_image_confirmed() &&
      millis() > kUnconfirmedDeadlineMs) {
    Serial.println("[OTA] unconfirmed image never reached broker; restarting");
    Serial.flush();
    esp_restart();
  }
  delay(2);
}
