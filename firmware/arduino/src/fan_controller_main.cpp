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
#include <Wire.h>

#include <cmath>

#include "config.h"
#include "esp_task_wdt.h"
#include "fan/control.h"
#include "net/mqtt_link.h"
#include "net/plug.h"
#include "sensors/air.h"
#include "net/weather.h"
#include "net/web.h"
#include "net/wifi_link.h"
#include "sensors/battery.h"
#include "sensors/climate.h"
#include "storage/history.h"
#include "storage/sdcard.h"
#include "system/coredump.h"
#include "system/crashlog.h"
#include "system/crumb_ring.h"
#include "system/eventlog.h"
#include "system/reboot.h"
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
  // Battery first and unconditionally: the charging verdict feeds the climate
  // correction (climate.h documents the dependency), so a wedged BME280 must
  // not freeze the hysteresis, the voltage slope or the runtime estimate.
  const uint32_t t0 = millis();
  battery::sample();
  const uint32_t t_batt = millis();
  float t, h, p;
  const bool have = climate::sample(&t, &h, &p);
  const uint32_t t_bme = millis();
  uint32_t t_sd = t_bme;
  if (have) {
    const float oc = climate::outside_c_fresh();
    history::Sample row;
    row.t = t;
    row.h = h;
    row.p = p;
    row.out_f = isnan(oc) ? NAN : oc * 9 / 5 + 32;
    row.speed = (int8_t)(fan::speed() < 0 ? 0 : fan::speed());
    row.batt_v = battery::kind() ? battery::volts() : NAN;
    row.chg = battery::kind() ? (battery::charging() ? 1 : 0) : -1;
    // The plug reading is only fresh-ish (15 s poll); a minute-old value is
    // NAN here so the chart shows a hole rather than a stale flat line.
    row.watts = (plug::age_s() >= 0 && plug::age_s() < 60) ? plug::watts() : NAN;
    row.voc_raw = air::voc_raw();
    row.nox_raw = air::nox_raw();
    row.voc = (int16_t)air::voc_index();
    row.nox = (int16_t)air::nox_index();
    history::append(row, time_synced());
    if (time_synced())
      // row.speed, not fan::speed(): the ring stores the clamped value and the
      // CSV used to store the raw one, so a sample taken during an /api/raw
      // sweep left the two records of the same instant disagreeing (0 vs -1).
      sdcard::log_sample(time(nullptr), t, h, p, row.out_f, row.speed, row.batt_v, row.chg,
                         row.watts, row.voc_raw, row.nox_raw, row.voc, row.nox);
    t_sd = millis();
    mqtt_link::publish_climate(t, h, p);
  }
  // Anything in here that blocks past the 15 s MQTT keepalive silently kills
  // the broker session. Deployed 1.14.8 stalled the whole loop ~16 s at every
  // sample: the broker fired the fan's will ("availability: offline") and the
  // reconnect looked like a random dropout to every subscriber, while
  // publish_climate -- reached after the stall, with the session already gone
  // -- dropped its reading on the floor. Logged on EVERY exit path, phase by
  // phase, so the tape names the slow step instead of implicating the loop
  // (2026-08-09).
  const uint32_t total = millis() - t0;
  if (total > 2000)
    eventlog::log("slow", "sample %lums batt=%lu bme=%lu sd=%lu", (unsigned long)total,
                  (unsigned long)(t_batt - t0), (unsigned long)(t_bme - t_batt),
                  (unsigned long)(t_sd - t_bme));
}

void setup() {
  pinMode(FAN_PWM_PIN, OUTPUT);
  digitalWrite(FAN_PWM_PIN, LOW);  // off on this rig = line low
  pinMode(FAN_SPARE_PIN, OUTPUT);
  digitalWrite(FAN_SPARE_PIN, HIGH);
  Serial.begin(115200);
  // Task watchdog, armed BEFORE anything that can wedge. A frozen loop (hung
  // bus op, wedged driver) force-reboots in 60 s instead of hanging dark
  // forever. The fan rides through on RMT. 60 s tolerates worst-case framework
  // writes to one stalled peer (10 s per NetworkClient::write call) on the
  // small JSON endpoints while still catching genuine hangs. Large payloads
  // use http_tx and never stall.
  //
  // It is FIRST for a reason. It used to sit ~35 lines down, after the NVS
  // reads below and after a core-dump probe -- and on 2026-08-11 that probe
  // hung on a board whose partition table has no coredump slot. Boot never
  // reached this line, so the watchdog never armed, so the board never
  // rebooted, so ota_rollback never got its unhealthy boot. It bricked, and
  // recovery took a USB cable. The rollback guard only fires on a REBOOT: a
  // hang before the watchdog is running is invisible to it, and everything
  // above this line is unrecoverable over the air. Keep this block first, and
  // keep the code above it to pin safe-states only.
  esp_task_wdt_config_t wdt_cfg = {.timeout_ms = 60000, .idle_core_mask = 0, .trigger_panic = true};
  if (esp_task_wdt_init(&wdt_cfg) != ESP_OK)
    esp_task_wdt_reconfigure(&wdt_cfg);
  esp_task_wdt_add(NULL);
  eventlog::capture_esp_logs();
  ota_rollback_check_at_boot();
  g_prefs.begin("fanctl", false);
  battery::restore(&g_prefs);
  climate::restore(&g_prefs);
  // Announce speed changes over whatever transports are up. Registered before
  // fan::restore so even the boot-time resume publishes once MQTT connects.
  fan::set_notify([](int speed, bool from_mqtt) {
    eventlog::log("fan", "speed=%d src=%s", speed, from_mqtt ? "mqtt" : "local");
    mqtt_link::publish_state(speed);
    if (!from_mqtt)
      mqtt_link::echo_set(speed);
    web::push_state();
  });
  fan::restore(&g_prefs);
  odometer::restore(&g_prefs);
  sdcard::set_quarantined(crashlog::examine_boot(&g_prefs));
  // The flight recorder's first line: what killed the previous run. This is
  // the line that turns "it randomly rebooted overnight" into a diagnosis.
  eventlog::log("boot", "fw=%s cause=%s prev=%s boots=%lu", kFwVersion, crashlog::last_death(),
                crashlog::prev_death(), (unsigned long)crashlog::boots());
  // The last steps of the run that just ended. Pure RTC reads -- no driver, no
  // flash, nothing that can wedge a boot. This is what the single crumb could
  // not give: the approach to a fault, not just an operation caught in flight.
  {
    char trail[220];
    crumb_ring::render(trail, sizeof(trail));
    if (trail[0] != '\0')
      eventlog::log("trail", "%s", trail);
  }
  crumb_ring::reset();
  TRAIL("setup");
  if (sdcard::quarantined())
    eventlog::log("sd", "previous boot died in an SD op -- card quarantined");
  Wire.begin();
  air::begin();  // probe the STEMMA chain while the bus is quiet
  SPI.begin();
  display::begin();
  // SD stays OUT of the boot path (first mount ~60 s later from loop): a
  // 1.14.3 experiment mounted here, before WiFi, to give esp_vfs_fat_register
  // its contiguous slab -- and the radio then failed to come up at all; the
  // board needed the boot-health rollback to recover (2026-08-09). The heap
  // is a fixed pie: the mount's slab comes from trimmed buffers (eventlog
  // ring, /api/events bounce buffer), not from boot-order games.
  wifi_link::begin();
  mqtt_link::init();
  web::begin(&g_prefs);
  Serial.printf("garage fan controller %s: waiting for wifi\n", kFwVersion);
}

void loop() {
  air::tick();  // 1 Hz SGP41 cadence, self rate-limited; ~10 ms when it runs
  if (wifi_link::connected() && !g_services_up) {
    g_services_up = true;
    // Mount the card before the web server takes clients: sdcard.h explains
    // why this exact moment -- after the radio (1.14.3's lesson), before web
    // traffic fragments the heap past the filesystem's contiguous need.
    sdcard::on_network_up();
    // Crash forensics AFTER the radio, never before it. Run in setup() this
    // cost the network on a since-fixed release, and the network is the only thing that can
    // deliver a fix. Nothing here is load-bearing enough to justify sitting in
    // front of it.
    coredump::log_at_boot();
    MDNS.begin(FAN_HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    web::start();
    Serial.printf("web ui: http://%s.local/\n", FAN_HOSTNAME);
  }
  // Feed BETWEEN the blocking stages, not just once at the top.
  //
  // One iteration can contain a synchronous MQTT connect (up to PubSubClient's
  // 15 s socket timeout), a ~17 s e-ink refresh, and -- in the 30 s branch
  // below -- a weather fetch whose DNS resolution can burn ~14 s when the
  // resolver blackholes. A router reboot produces all three at once: ~45-55 s
  // against a 60 s trigger_panic watchdog, with nothing between them to feed
  // it. Each of these is individually bounded well under the limit; only the
  // accumulation was dangerous, so a reset after each stage removes it.
  esp_task_wdt_reset();
  wifi_link::tick();
  TRAIL("mqtt");
  mqtt_link::tick();
  esp_task_wdt_reset();
  TRAIL("web");
  web::handle();
  odometer::tick(fan::speed(), fan::watts(fan::speed()));
  esp_task_wdt_reset();
  TRAIL("epd");
  display::maybe_render();
  esp_task_wdt_reset();
  if (g_last_sample_ms == 0 || millis() - g_last_sample_ms >= kSampleMs) {
    g_last_sample_ms = millis();
    TRAIL("sample");
    sample_climate();
    web::push_state();
    // The heartbeat the disconnect forensics read backwards from: the last
    // health line before a gap dates the death and carries the vitals
    // (signal, heap) that usually explain it.
    // largest = biggest contiguous free block. Free heap alone lies on this
    // board: 20 KB "free" could not fit the SD filesystem's 13 KB (2026-08-09).
    // stack= is the loop task's remaining headroom, in bytes, at its worst so
    // far. It is here because the first SD-purge panic was a stack overflow
    // (3 KB of batch buffer per recursion level against an 8 KB stack) and
    // nothing on the tape hinted at it -- the fault was found by reading the
    // source afterwards. A number that walks toward zero says so in advance.
    eventlog::log("health", "rssi=%d heap=%lu largest=%lu min_heap=%lu stack=%lu drops=%lu",
                  wifi_link::rssi(), (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  (unsigned long)ESP.getMinFreeHeap(),
                  (unsigned long)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
                  (unsigned long)wifi_link::drops());
  }
  if (millis() - g_last_auto_ms >= kAutoTickMs) {
    g_last_auto_ms = millis();
    // Serial-only heap trace: the 5-minute health line is the tape record;
    // this is the bench view for catching a leak's slope between heartbeats.
    // CDC drops it silently when no terminal is attached.
    Serial.printf("[heap] free=%lu largest=%lu min=%lu psram=%lu\n",
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  (unsigned long)ESP.getMinFreeHeap(), (unsigned long)ESP.getPsramSize());
    fan::tick_auto();
    battery::begin();
    battery::read();
    sdcard::retry_tick();
    eventlog::flush_tick();
    weather::tick();       // internally rate-limited to one fetch per 10 min
    plug::tick();          // watt-meter poll, rate-limited to one per 15 s
    esp_task_wdt_reset();  // a blackholed resolver makes those fetches long ones
  }
  if (!mqtt_link::ever_connected() && !ota_rollback_image_confirmed() &&
      millis() > kUnconfirmedDeadlineMs) {
    sysreboot::restart("unconfirmed image never reached the broker; restarting");
  }
  delay(2);
}
