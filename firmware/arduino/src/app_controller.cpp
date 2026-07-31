// Application controller implementation
#include "app_controller.h"
#include <ESPmDNS.h>
#include <esp_task_wdt.h>  // Hardware watchdog
#include <time.h>
#include <cmath>
#include <cstdio>
#include "config.h"
#include "runtime_config.h"
#include "generated_config.h"
#include "sensors.h"
#include "power.h"
#include "wifi_manager.h"
#include "mqtt_client.h"
#include "net.h"
#include "system_manager.h"
#include "state_manager.h"
#include "metrics_diagnostics.h"
#include "display_manager.h"
#if USE_DISPLAY
#include "display_renderer.h"
#endif
#include "crash_handler.h"
#include "memory_tracking.h"
#include "mqtt_batcher.h"
#include "profiling.h"
#include "ota_manager.h"
#include "sd_store.h"
#include "logging/logger.h"
#include "history_ring.h"

// Diagnostic test functions (from diagnostic_test.cpp)
extern void diagnostic_test_init();
extern void diagnostic_test_loop();
extern void show_boot_stage(int stage);
extern void pixel_flash(uint8_t r, uint8_t g, uint8_t b, uint16_t ms);
extern void pixel_tick();

// Set from the MQTT cmd/page handler; consumed in app_loop.
static volatile bool g_page_flip_request = false;
void request_page_flip() { g_page_flip_request = true; }

// Track wake time for phase management
static uint32_t g_wake_time_ms = 0;

// Diagnostic mode tracking
static uint32_t g_diagnostic_last_publish_ms = 0;
#define DIAGNOSTIC_PUBLISH_INTERVAL_MS 30000

#if ALWAYS_ON
// --- always-on scheduling state ---------------------------------------------
// Timestamps are millis()-based and compared with subtraction so the 49-day
// rollover takes care of itself.
static uint32_t g_last_sample_ms = 0;
static uint32_t g_last_display_ms = 0;
static uint32_t g_last_link_check_ms = 0;
static uint32_t g_last_maintenance_ms = 0;
static uint32_t g_sample_count = 0;
static bool g_display_drawn_once = false;

// Values as of the last panel draw, used to skip redundant refreshes.
static float g_drawn_tempC = NAN;
static float g_drawn_rhPct = NAN;
static int g_drawn_batt_pct = -1;

// Values as of the last panel draw for the outside half of the screen.
static float g_drawn_outside_tempC = NAN;

// How often to re-check WiFi/MQTT health.
#define LINK_CHECK_INTERVAL_MS 30000UL
// Cadence for uptime accounting, the NVS commit and the retention sweep.
#define MAINTENANCE_INTERVAL_MS (6UL * 3600UL * 1000UL)

#if USE_DISPLAY
static void maybe_refresh_display();
#endif
#endif  // ALWAYS_ON

// History is written by both builds: the deep-sleep node is the one whose data
// nobody can retrieve after the fact, so a local CSV is worth more there, not
// less. The always-on loop calls this once per sample interval; the deep-sleep
// build calls it once per wake, which is the same thing.
static void record_sample_to_sd();
#if FEATURE_SD_STORAGE && !ALWAYS_ON
static void maybe_prune_history();
#endif

// mDNS/OTA hostname. Derived unconditionally during setup rather than inside the
// WiFi-connected branch: if WiFi is down at boot and only comes up later, the
// OTA listener still needs a resolvable name to advertise.
static char g_hostname[33] = {0};

static void derive_hostname() {
  String hostname = String(rc_room_name());
  hostname.toLowerCase();
  hostname.replace(" ", "-");
  snprintf(g_hostname, sizeof(g_hostname), "%s", hostname.c_str());
}

// Publish the retained Home Assistant discovery configs, once per boot. Split
// from the connect path because MQTT can come up either during setup or — after
// a power-outage boot race — from the link-check loop, and whichever happens
// first should publish. Without this call HA never auto-creates the entities:
// the only other caller of ha_discovery_publish_all() was net_init_and_connect(),
// which nothing invokes.
static void publish_ha_discovery_once() {
  static bool s_published = false;
  if (s_published || !mqtt_is_connected())
    return;
  s_published = true;
  net_publish_ha_discovery();
  Serial.println("[NET] HA discovery published");
}

// Bring up mDNS and the OTA listener. Safe to call repeatedly -- ota_begin()
// returns immediately once running -- so callers can just retry while WiFi is up.
static void start_network_services() {
  if (!wifi_is_connected())
    return;

  // mDNS success is tracked on its own rather than inferred from OTA state: the
  // old guard (`!ota_is_active()`) meant a boot-time mDNS failure became
  // permanent the moment OTA came up, and <room>.local never resolved again.
  // Failures after the first are retried silently -- at a 30 s cadence a
  // persistent failure would otherwise print 2880 lines a day.
  static bool s_mdns_ok = false;
  static bool s_mdns_fail_logged = false;
  if (!s_mdns_ok) {
    if (MDNS.begin(g_hostname)) {
      s_mdns_ok = true;
      Serial.printf("[NET] mDNS started: %s.local\n", g_hostname);
      MDNS.addService("espsensor", "tcp", 80);
      MDNS.addServiceTxt("espsensor", "tcp", "version", FW_VERSION);
      MDNS.addServiceTxt("espsensor", "tcp", "room", rc_room_name());
    } else if (!s_mdns_fail_logged) {
      s_mdns_fail_logged = true;
      Serial.println("[NET] mDNS failed to start (will keep retrying quietly)");
    }
  }

  // After mDNS, so the OTA service record lands in the responder set up above
  // instead of ArduinoOTA re-initialising mDNS and discarding it.
  ota_begin(g_hostname);

#if ALWAYS_ON
  // Modem sleep between DTIM beacons. The radio at full power is the largest
  // single heat source on the board; with it duty-cycled, MQTT and OTA still
  // work — packets just wait for the next beacon (tens of ms). Set here rather
  // than at connect time because this function is the "network is up" hook and
  // is safe to call repeatedly.
  wifi_configure_power_save(true);
  wifi_reduce_tx_power();
#endif
}

uint32_t get_wake_time_ms() { return g_wake_time_ms; }

// millis() value up to which this session has already been counted.
static uint32_t g_uptime_accounted_ms = 0;

// Bring the cumulative uptime counter up to date with this session.
//
// The deep-sleep build could simply add millis()/1000 once, just before
// sleeping. An always-on node never reaches that point, so uptime has to be
// accounted incrementally, and tracking what has already been counted is what
// keeps repeated calls from inflating the total.
//
// Kept in the millis() domain rather than in seconds: unsigned subtraction is
// correct across the ~49.7-day rollover, which an always-on node WILL reach. A
// seconds-based "now > last_counted" comparison silently stops being true after
// the wrap and freezes the counter for good -- the deep-sleep build never hits
// that because it reboots every cycle. Only whole seconds are consumed, so the
// sub-second remainder carries forward instead of being repeatedly discarded.
static void account_uptime() {
  uint32_t elapsed_ms = millis() - g_uptime_accounted_ms;
  uint32_t whole_s = elapsed_ms / 1000;
  if (whole_s == 0)
    return;

  add_to_cumulative_uptime(whole_s);
  g_uptime_accounted_ms += whole_s * 1000;
}

bool is_first_boot() { return esp_reset_reason() == ESP_RST_POWERON; }

// Main application setup orchestration
void app_setup() {
  g_wake_time_ms = millis();

#if ALWAYS_ON
  // BOOT (GPIO0) doubles as the page-flip button at runtime. Strapping pin:
  // only sampled at reset, so a runtime press is safe; INPUT_PULLUP matches its
  // active-low wiring.
  pinMode(0, INPUT_PULLUP);

  // 80 MHz, not the 240 MHz default. The deep-sleep build races to finish and
  // sleep, but an always-on node runs forever and every milliwatt becomes heat
  // that the BME280 — millimetres away — reads as room temperature. WiFi, SPI
  // and I2C all run from the 80 MHz APB clock, which this does not change.
  setCpuFrequencyMhz(80);
#endif

  // Initialize serial FIRST with longer delay
  Serial.begin(115200);
  delay(500);  // Longer delay for serial stability

  // Initialize hardware watchdog early (30 second timeout)
  // This catches hangs in setup - will reboot if setup takes too long
  esp_task_wdt_init(30, true);  // 30 sec timeout, panic (reboot) on timeout
  esp_task_wdt_add(NULL);       // Add current task to watchdog

  // Bring up the structured logger before anything logs through it. This call
  // was missing entirely, which left every sink dead: no ring buffer for crash
  // dumps, no NVS persistence, no MQTT log stream — LOGM_* went to serial only.
  // With the USB cable gone, the MQTT sink (espsensor/<id>/logs/#) is the only
  // live window into a misbehaving device, so it is on; its own rate limiter
  // keeps a log storm from starving sensor publishes. NVS keeps ERROR+ for
  // post-mortem after a crash.
  {
    Logger::Config log_config;
    log_config.buffer_enabled = true;
    log_config.nvs_enabled = true;
    log_config.mqtt_enabled = true;
    Logger::getInstance().begin(log_config);
  }

  // Classify this reset and bump the RTC boot/crash counters. This was never
  // called, so /debug/boot_count and /debug/crash_count published 0 forever and
  // a watchdog reboot-loop in the field would have been invisible in telemetry.
  update_boot_counters();

  // Immediate debug output
  Serial.println("\n\n=== ESP32 BOOT SEQUENCE ===");
  Serial.printf("FW Version: %s\n", FW_VERSION);
#ifdef BOOT_DEBUG
  Serial.println("Build: dev_display");
  Serial.println("Flags: DEV_NO_SLEEP, FORCE_FULL_ONLY, BOOT_DEBUG");
#endif
  Serial.flush();
  delay(10);
  Serial.println("[BOOT-1] Serial initialized");
  Serial.flush();
  show_boot_stage(1);  // Red for boot/serial

// Initialize crash handler and memory tracking early
#if FEATURE_CRASH_HANDLER
  CrashHandler::getInstance().begin();
  // Check for crash info from previous boot
  if (CrashHandler::getInstance().hasCrashInfo()) {
    Serial.println("[BOOT-1a] ⚠️  Previous crash detected!");
    char report[256];
    CrashHandler::getInstance().formatCrashReport(report, sizeof(report));
    Serial.println(report);
    // Note: Will publish via MQTT after connection
  }
#endif

#if FEATURE_MEMORY_TRACKING
  MemoryTracker::getInstance().begin();
#endif

  // Boot feedback is show_boot_stage() (diagnostic_test.cpp), already called at
  // every stage. The analogWrite "red flash" that used to sit here was PWM into
  // a NeoPixel data line — a protocol-driven part that PWM cannot light.

  Serial.println("[2] Starting initialization");
  Serial.flush();

// Run diagnostic tests in DEV_NO_SLEEP mode
#if DEV_NO_SLEEP
  Serial.println("[BOOT-2a] Running hardware diagnostics...");
  diagnostic_test_init();
  Serial.println("[BOOT-2b] Diagnostics complete, continuing boot...");
  Serial.flush();
#endif

#ifdef BOOT_DEBUG
  show_boot_stage(2);  // Yellow for display init
#endif

  // Seed the effective config from the compiled-in values before ANY consumer
  // runs. display_manager_init() draws the header, which reads rc_room_name();
  // with this call after it, that read hit a zero-initialised struct and the
  // boot screen showed an empty room name. rc_begin() only copies macros, so it
  // has nothing to wait for.
  Serial.println("[2d] Resolving configuration...");
  rc_begin();

#if USE_DISPLAY
  Serial.println("[BOOT-2c] Initializing display...");
  display_manager_init();  // This will show "12:34" test pattern in debug mode
  Serial.println("[BOOT-2c] Display initialized");
#endif

  // The card is mounted only now: it shares the SPI bus with the panel, and the
  // display driver is what brings that bus up. Overrides therefore land after
  // the boot screen has already drawn -- the compiled-in room name is what shows
  // until the first post-config refresh.
#if FEATURE_SD_STORAGE
  if (sd_begin()) {
    sd_load_config();

    // Both log paths onto the card open here, once the card is known good and
    // the card's own logs_enabled setting has been read. Anything logged earlier
    // in boot went to serial only — there was nowhere else for it to go.
    sd_log_set_mirror(rc_logs_enabled());
    Logger::getInstance().enableSD(rc_logs_enabled());

    // Apply a card-staged firmware image before doing any real work: if it
    // succeeds this call reboots and never returns.
    if (sd_has_staged_update()) {
      Serial.println("[2e] Staged firmware image found on SD, applying...");
      ota_apply_from_sd();
      // Only reached when the image was rejected; ota_last_sd_result() says why.
      Serial.printf("[2e] SD update not applied: %s\n", ota_last_sd_result());
    }
  }
#endif
  rc_log_summary();

  // Initialize state management with error checking
  Serial.println("[3] Initializing NVS cache...");
  Serial.flush();
  nvs_begin_cache();
  nvs_load_cache_if_unset();
  Serial.println("[3] NVS cache OK");
  Serial.flush();

  // Initialize power management with error checking
  Serial.println("[4] Initializing power management...");
  power_init();
  power_wake_from_sleep();

  // Check battery status
  BatteryStatus bs = read_battery_status();
  Serial.printf("[4] Battery: %d%% (%.2fV)\n", bs.percent, bs.voltage);

  // Check for critical battery - properly cleanup before emergency sleep
  if (bs.percent >= 0 && bs.percent < 5) {
    Serial.println("Battery critical! Preparing for emergency deep sleep...");

    // Prepare hardware for sleep (puts fuel gauge to sleep)
    power_prepare_sleep();

    // Save any cached state to NVS
    nvs_end_cache();

#if FEATURE_SD_STORAGE
    sd_end();
#endif

    Serial.println("Entering emergency deep sleep (1 hour)");
    Serial.flush();

    // Sleep for 1 hour to conserve remaining battery
    esp_sleep_enable_timer_wakeup(3600ULL * 1000000ULL);
    esp_deep_sleep_start();
  }

  Serial.println("[4] Power management OK");

  // Initialize sensors with error checking
  Serial.println("[5] Initializing sensors...");
  Serial.flush();
  sensors_init_all();
  // Note: We continue even if some sensors fail
  // The sensors module will handle individual failures
  Serial.println("[5] Sensors initialized (check logs for any failures)");
  Serial.flush();

#ifdef BOOT_DEBUG
// In debug mode, show memory status after sensor init
#if FEATURE_MEMORY_TRACKING
  MemoryTracker::getInstance().update();
#endif
  Serial.printf("[MEMORY] After sensors - Free heap: %u, Min free: %u\n", ESP.getFreeHeap(),
                ESP.getMinFreeHeap());
#endif

  // Display init, storage mount and sensor bring-up above can consume ~8 s of
  // the 30 s watchdog budget; the WiFi backoff below can take 21 s on its own
  // when the AP is unreachable. Without a feed between them, a boot with the
  // router down trips the watchdog just before setup completes and the device
  // reboot-loops until the AP returns.
  esp_task_wdt_reset();

  // Initialize network with exponential backoff
  Serial.println("[BOOT-3] Attempting WiFi connection...");
  show_boot_stage(3);  // Blue for WiFi

  // Use new exponential backoff connection
  if (!wifi_connect_with_exponential_backoff(3, 1000)) {  // 3 attempts, 1s initial delay
    Serial.println("[BOOT-3] WiFi connection failed - continuing anyway");
    // Set time from compile timestamp as fallback (better than epoch)
    wifi_set_time_from_compile();
    show_boot_stage(5);  // Purple for error
  } else {
    Serial.printf("[BOOT-4] WiFi connected - IP: %s, RSSI: %d\n", wifi_get_ip().c_str(),
                  wifi_get_rssi());
    show_boot_stage(4);  // Green for ready
  }

  // mDNS hostname comes from the room name (spaces to dashes, lowercase) and is
  // derived whether or not WiFi came up, so a later reconnect has it ready.
  derive_hostname();
  start_network_services();

  // Initialize MQTT. Through net_begin(), not mqtt_begin() directly: net_begin
  // is what derives the MAC-based client id and registers it with the MQTT and
  // HA-discovery modules. Calling mqtt_begin() bare left the id empty, which
  // silently disabled every sensor publish (the queue guards on a non-empty id)
  // and put the LWT on espsensor/unknown/.
  if (wifi_is_connected()) {
    net_begin();
    ensure_mqtt_connected();
    publish_ha_discovery_once();

#if FEATURE_CRASH_HANDLER
    // The crash detected at boot finally gets shipped. It was formatted and
    // printed above with a "will publish via MQTT after connection" note, but
    // nothing ever did — mqtt_publish_last_crash() had no callers. Cleared only
    // after a successful publish so an offline boot retries next time.
    if (mqtt_is_connected() && CrashHandler::getInstance().hasCrashInfo()) {
      char crash_report[256];
      CrashHandler::getInstance().formatCrashReport(crash_report, sizeof(crash_report));
      mqtt_publish_last_crash(crash_report);
      CrashHandler::getInstance().clearCrashInfo();
      Serial.println("[NET] Previous crash report published");
    }
#endif
  }

  // The WiFi + MQTT block above can spend ~25 s against an unreachable AP and a
  // half-dead broker; give the boot phases below a fresh watchdog budget.
  esp_task_wdt_reset();

  // Run main phases
  run_sensor_phase();
  run_network_phase();

#if USE_DISPLAY
  run_display_phase();
#endif

#if FEATURE_SD_STORAGE
  // Refill the sparkline ring from the CSVs before the boot sample lands, so a
  // reboot (every OTA deploy) no longer blanks the graphs page for hours.
  // After the network phase because file selection needs the NTP clock.
  sd_backfill_history([](float tC, float rh, float otC, float orh) {
    hist_push(isfinite(tC) ? tC * 9.0f / 5.0f + 32.0f : NAN,
              isfinite(otC) ? otC * 9.0f / 5.0f + 32.0f : NAN, rh, orh);
  });
#endif

  // After the network phase, so the row carries an NTP-corrected timestamp and a
  // real RSSI rather than the placeholders the sensor phase starts with.
  record_sample_to_sd();
#if FEATURE_SD_STORAGE && !ALWAYS_ON
  maybe_prune_history();
#endif

#if ALWAYS_ON
  // Seed the "what is on screen" snapshot from the draw just made, and start the
  // interval clocks so the loop waits a full interval before the next sample.
  uint32_t now = millis();
  g_last_sample_ms = now;
  g_last_display_ms = now;
  g_last_link_check_ms = now;
  g_last_maintenance_ms = now;
  g_sample_count = 1;
  g_display_drawn_once = true;
  g_drawn_tempC = get_last_published_inside_tempC();
  g_drawn_rhPct = get_last_published_inside_rh();
  g_drawn_batt_pct = read_battery_status().percent;

  Serial.printf("=== Always-on mode: sampling every %u s, panel redraw floor %d s ===\n",
                rc_sample_interval_sec(), DISPLAY_MIN_REFRESH_INTERVAL_SEC);
#endif

  // Boot finished: douse the stage LED. Leaving it lit costs battery around the
  // clock and makes "green" meaningless as a boot-progress signal.
  show_boot_stage(0);

  run_sleep_phase();
}

// Persist one row of sensor history to the card. Silent no-op when the card is
// absent or history is turned off in config.
static void record_sample_to_sd() {
#if FEATURE_SD_STORAGE
  if (!sd_is_mounted() || !rc_history_enabled())
    return;

  BatteryStatus bs = read_battery_status();
  OutsideReadings out_now = net_get_outside();
  sd_append_history(
      time(nullptr), millis() / 1000, get_last_published_inside_tempC(),
      get_last_published_inside_rh(), get_last_published_inside_pressureHPa(), bs.voltage,
      bs.percent, wifi_is_connected() ? wifi_get_rssi() : 0,
      (out_now.validTemp && isfinite(out_now.temperatureC)) ? out_now.temperatureC : NAN,
      (out_now.validHum && isfinite(out_now.humidityPct)) ? out_now.humidityPct : NAN);
#endif

  // Feed the 24h sparkline ring (display-ready units: F / %). Outside values
  // only when currently valid; NaN renders as a gap.
  {
    const float tin = get_last_published_inside_tempC();
    OutsideReadings o = net_get_outside();
    hist_push(
        isfinite(tin) ? tin * 9.0f / 5.0f + 32.0f : NAN,
        (o.validTemp && isfinite(o.temperatureC)) ? o.temperatureC * 9.0f / 5.0f + 32.0f : NAN,
        get_last_published_inside_rh(),
        (o.validHum && isfinite(o.humidityPct)) ? o.humidityPct : NAN);
  }
#if USE_STATUS_PIXEL
  // Quiet cyan blip: a sample was just taken. Dim and brief on purpose.
  pixel_flash(0, 18, 18, 120);
#endif
}

#if FEATURE_SD_STORAGE && !ALWAYS_ON
// The always-on build sweeps expired history on its maintenance timer. The
// deep-sleep build has no such loop -- it wakes, works and sleeps -- so it sweeps
// at most once per calendar day instead. Pruning is a full directory scan, and
// doing one on every wake would be a real cost at a 5-minute cadence.
//
// The marker lives in RTC memory, which survives deep sleep but not a power
// cycle; the worst case on reboot is one extra scan.
RTC_DATA_ATTR static long rtc_last_prune_day = -1;

static void maybe_prune_history() {
  if (!sd_is_mounted())
    return;

  time_t now = time(nullptr);
  // Without NTP there is no way to tell which files are old, and recording a day
  // number derived from the RTC default would suppress the real sweep later.
  if (now <= SD_MIN_PLAUSIBLE_EPOCH)
    return;

  long today = static_cast<long>(now / 86400);
  if (today == rtc_last_prune_day)
    return;
  rtc_last_prune_day = today;

  uint16_t pruned = sd_prune_history(rc_history_retention_days());
  if (pruned > 0) {
    Serial.printf("SD: pruned %u expired history file(s)\n", pruned);
  }
}
#endif  // FEATURE_SD_STORAGE && !ALWAYS_ON

#if ALWAYS_ON
#if USE_DISPLAY
// Decide whether the panel is worth redrawing.
//
// Sampling cadence and draw cadence are deliberately separate. A full refresh
// flashes the panel and spends one of its finite refresh cycles, so a draw needs
// both a meaningful change in the displayed values AND enough time since the
// last one.
static void maybe_refresh_display() {
  const float tempC = get_last_published_inside_tempC();
  const float rhPct = get_last_published_inside_rh();
  const int batt_pct = read_battery_status().percent;
  const OutsideReadings outside = net_get_outside();

  // A reading only counts as "changed" once it is valid; going from a valid
  // reading to NaN (a failed read) should not trigger a redraw that would just
  // blank the field.
  auto exceeded = [](float now_v, float drawn_v, float threshold) {
    if (!isfinite(now_v))
      return false;
    if (!isfinite(drawn_v))
      return true;
    return fabsf(now_v - drawn_v) >= threshold;
  };

  bool changed = false;
  if (exceeded(tempC, g_drawn_tempC, THRESH_TEMP_C))
    changed = true;
  if (exceeded(rhPct, g_drawn_rhPct, THRESH_RH_PCT))
    changed = true;
  if (outside.validTemp && exceeded(outside.temperatureC, g_drawn_outside_tempC, THRESH_TEMP_C))
    changed = true;
  // Battery is shown as a coarse indicator, so only sizeable steps matter.
  // Written as an explicit range test rather than abs(), which Arduino.h defines
  // as a macro that misbehaves on non-trivial arguments.
  if (batt_pct >= 0 && g_drawn_batt_pct >= 0) {
    const int delta = batt_pct - g_drawn_batt_pct;
    if (delta >= 5 || delta <= -5)
      changed = true;
  }

  const uint32_t now = millis();
  const uint32_t floor_ms = DISPLAY_MIN_REFRESH_INTERVAL_SEC * 1000UL;
  const bool floor_elapsed = !g_display_drawn_once || (now - g_last_display_ms) >= floor_ms;

  if (!floor_elapsed) {
    if (changed) {
      Serial.printf("Display: change pending, holding off (%lu s of %d s floor elapsed)\n",
                    (now - g_last_display_ms) / 1000UL, DISPLAY_MIN_REFRESH_INTERVAL_SEC);
    }
    return;
  }

  // At each floor interval the display alternates between the live page and the
  // 24h graphs page, so every refresh cycle is spent showing something new --
  // this replaces the old "skip when unchanged" rule, whose purpose (not
  // burning refreshes on identical frames) the alternation now serves.
  display_toggle_page();
  run_display_phase();

  g_last_display_ms = now;
  g_display_drawn_once = true;
  g_drawn_tempC = tempC;
  g_drawn_rhPct = rhPct;
  g_drawn_batt_pct = batt_pct;
  if (outside.validTemp)
    g_drawn_outside_tempC = outside.temperatureC;
}
#endif  // USE_DISPLAY
#endif  // ALWAYS_ON

// Main application loop (for diagnostic mode)
void app_loop() {
  // Feed the hardware watchdog to prevent reboot
  esp_task_wdt_reset();

#if ALWAYS_ON
  // Service OTA first and unconditionally: it is the escape hatch if anything
  // below turns out to be broken in a shipped build.
  ota_loop();

  // A transfer runs inside ota_loop(); starting a sensor read or a multi-second
  // panel refresh underneath it would stall or fail the update.
  if (ota_in_progress()) {
    return;
  }

  const uint32_t now = millis();

#if USE_STATUS_PIXEL
  pixel_tick();
  // Idle twinkle: a soft blip every ~7 s says "alive" without lighting the
  // room. Rotates through three dim tints so it reads as a twinkle, not a
  // metronome. Duty cycle ~1%, so the battery cost is noise.
  {
    static uint32_t s_last_twinkle_ms = 0;
    static uint8_t s_phase = 0;
    if (now - s_last_twinkle_ms >= 7000) {
      s_last_twinkle_ms = now;
      switch (s_phase++ % 3) {
        case 0:
          pixel_flash(4, 10, 4, 70);  // soft green
          break;
        case 1:
          pixel_flash(4, 6, 12, 70);  // soft blue
          break;
        default:
          pixel_flash(8, 8, 8, 70);  // soft white
          break;
      }
    }
  }
#endif

#if USE_DISPLAY
  // BOOT (GPIO0) press = flip page immediately. Debounced edge; the 250 ms
  // guard also caps how fast a held button can burn e-ink refresh cycles.
  {
    static bool s_prev_high = true;
    static uint32_t s_edge_ms = 0;
    const bool level = digitalRead(0);
    if (s_prev_high && !level && (now - s_edge_ms) > 250) {
      s_edge_ms = now;
      g_page_flip_request = true;
    }
    s_prev_high = level;
  }
  if (g_page_flip_request) {
    g_page_flip_request = false;
    display_toggle_page();
#if USE_STATUS_PIXEL
    // Feedback: blue flash = live page, magenta = graphs page.
    if (display_current_page() == 0)
      pixel_flash(0, 0, 60, 250);
    else
      pixel_flash(60, 0, 60, 250);
#endif
    Serial.printf("[UI] page flip -> %u\n", display_current_page());
    run_display_phase();
    g_last_display_ms = now;
    g_display_drawn_once = true;
  }
#endif

  if (now - g_last_link_check_ms >= LINK_CHECK_INTERVAL_MS) {
    g_last_link_check_ms = now;

    if (!wifi_is_connected()) {
      Serial.println("[ALWAYS-ON] WiFi down, reconnecting...");
      if (wifi_connect_with_exponential_backoff(2, 500)) {
        Serial.printf("[ALWAYS-ON] WiFi back: %s\n", wifi_get_ip().c_str());
      }
    }

    // The reconnect above can block for ~17 s (backoff + NTP); start MQTT's
    // budget fresh so the two together cannot cross the 30 s watchdog.
    esp_task_wdt_reset();

    // Gate on whether the services are running, NOT on observing a reconnect
    // here. If WiFi was down at boot and the IDF stack reconnected on its own,
    // this check only ever sees an already-connected link, no transition
    // happens, and OTA would stay dead for the whole session -- precisely when
    // it is most needed. start_network_services() is cheap to retry.
    if (wifi_is_connected()) {
      start_network_services();

      // net_begin() is idempotent and normally ran during setup — but if WiFi
      // was down at boot (power outage: the ESP boots faster than the router),
      // setup skipped it and MQTT was never configured. Without this call the
      // node would retry a null broker address every 30 s forever.
      net_begin();
      ensure_mqtt_connected();
      publish_ha_discovery_once();
    }
  }

  // Pump MQTT so retained outside data and commands are handled between samples.
  net_loop();

  bool diag_request = false;
  if (net_check_diagnostic_mode_request(diag_request)) {
    set_diagnostic_mode(diag_request);
    mqtt_publish_diagnostic_mode(diag_request);
    Serial.printf("DIAG: Mode changed to %s via MQTT\n", diag_request ? "active" : "inactive");
  }

  const uint32_t interval_ms = rc_sample_interval_sec() * 1000UL;
  if (now - g_last_sample_ms >= interval_ms) {
    g_last_sample_ms = now;
    g_sample_count++;

    Serial.printf("\n=== Sample %u (uptime %lu s) ===\n", g_sample_count, millis() / 1000UL);

    run_sensor_phase();
    run_network_phase();
    record_sample_to_sd();
#if USE_DISPLAY
    maybe_refresh_display();
#endif

    // Periodic maintenance. An always-on node never reaches run_sleep_phase(),
    // which is where the deep-sleep build accounts uptime and closes the NVS
    // handle, so both have to happen on a timer here instead.
    if (now - g_last_maintenance_ms >= MAINTENANCE_INTERVAL_MS) {
      g_last_maintenance_ms = now;
      account_uptime();
      // Both had accumulated state that nothing ever read out: the error
      // counters grew in RTC memory unpublished, and crash reports always
      // showed min_free_heap=0 because the sampler was never called.
      publish_error_stats();
#if FEATURE_CRASH_HANDLER
      CrashHandler::getInstance().updateHeapStats();
#endif
      // Close and reopen so anything buffered in the Preferences handle is
      // committed rather than sitting there until a brownout discards it.
      nvs_end_cache();
      nvs_begin_cache();
#if FEATURE_SD_STORAGE
      uint16_t pruned = sd_prune_history(rc_history_retention_days());
      if (pruned > 0) {
        Serial.printf("SD: pruned %u expired history file(s)\n", pruned);
      }
#endif
    }
  }

  // Yield to the WiFi/TCP stacks. Short enough that OTA stays responsive.
  delay(20);
  return;
#endif  // ALWAYS_ON

#if DEV_NO_SLEEP
  // In always-on mode, just print alive message periodically
  static uint32_t last_print = 0;
  if (millis() - last_print > 5000) {
    last_print = millis();
    Serial.print("[ALIVE] Uptime: ");
    Serial.print(millis() / 1000);
    Serial.println(" seconds");
    Serial.flush();
  }
  delay(100);  // Small delay to prevent watchdog issues
  return;      // Don't run diagnostic mode logic in DEV_NO_SLEEP
#endif

  // Check for MQTT diagnostic mode commands
  bool diag_mode_value;
  if (net_check_diagnostic_mode_request(diag_mode_value)) {
    set_diagnostic_mode(diag_mode_value);
    mqtt_publish_diagnostic_mode(diag_mode_value);
    Serial.printf("DIAG: Mode changed to %s via MQTT\n", diag_mode_value ? "active" : "inactive");
  }

  // In diagnostic mode, stay awake and publish diagnostics periodically
  if (is_diagnostic_mode_active()) {
    // Keep network alive
    net_loop();

    // Diagnostic mode is the one place a deep-sleep build stays awake long
    // enough to accept a network update, so service OTA here too. Without this
    // the listener started at boot would never be polled.
    ota_loop();
    if (ota_in_progress()) {
      return;
    }

    // Publish diagnostics every interval
    if (millis() - g_diagnostic_last_publish_ms >= DIAGNOSTIC_PUBLISH_INTERVAL_MS) {
      g_diagnostic_last_publish_ms = millis();

      if (mqtt_is_connected()) {
        Serial.println("DIAG: Publishing diagnostic data");

        // Update and publish memory diagnostics
        MemoryDiagnostics mem_diag = get_memory_diagnostics();
        net_publish_memory_diagnostics(mem_diag.free_heap, mem_diag.min_free_heap,
                                       mem_diag.largest_free_block, mem_diag.fragmentation_pct);

        // Update uptime. account_uptime() folds this session in, so the counter
        // already includes it -- adding millis() again here would double count.
        account_uptime();
        uint32_t current_uptime = get_cumulative_uptime_sec();
        net_publish_uptime(current_uptime);

        // Publish other diagnostic info
        net_publish_wake_count(get_wake_count());
        net_publish_wifi_rssi(WiFi.RSSI());

        // Publish detailed diagnostic JSON
        char diag_json[512];
        snprintf(diag_json, sizeof(diag_json),
                 "{\"diag_mode\":true,\"free_heap\":%u,\"min_heap\":%u,"
                 "\"fragmentation\":%.1f,\"rssi\":%d,\"uptime\":%u,"
                 "\"boot_count\":%u,\"crash_count\":%u,\"wake_count\":%u}",
                 mem_diag.free_heap, mem_diag.min_free_heap, mem_diag.fragmentation_pct,
                 WiFi.RSSI(), current_uptime, get_boot_count(), get_crash_count(),
                 get_wake_count());
        net_publish_debug_json(diag_json, false);

        // Log to serial
        Serial.printf("DIAG: Heap: free=%u min=%u frag=%.1f%%\n", mem_diag.free_heap,
                      mem_diag.min_free_heap, mem_diag.fragmentation_pct);
      }
    }

    // Handle serial commands for diagnostic mode
    if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      cmd.trim();

      if (cmd == "normal" || cmd == "exit") {
        Serial.println("DIAG: Exiting diagnostic mode");
        set_diagnostic_mode(false);
        mqtt_publish_diagnostic_mode(false);
      } else if (cmd == "memtest") {
        Serial.println("DIAG: Running memory test");
        MemoryDiagnostics mem = get_memory_diagnostics();
        Serial.printf("  Free: %u bytes\n", mem.free_heap);
        Serial.printf("  Min:  %u bytes\n", mem.min_free_heap);
        Serial.printf("  Largest block: %u bytes\n", mem.largest_free_block);
        Serial.printf("  Fragmentation: %.1f%%\n", mem.fragmentation_pct);
      } else if (cmd == "sensortest") {
        Serial.println("DIAG: Testing sensors");
        InsideReadings ir = read_inside_sensors();
        Serial.printf("  Temp: %.2f°C\n", ir.temperatureC);
        Serial.printf("  Humidity: %.1f%%\n", ir.humidityPct);
        Serial.printf("  Pressure: %.1f hPa\n", ir.pressureHPa);
      } else if (cmd == "wifitest") {
        Serial.println("DIAG: WiFi scan");
        int n = WiFi.scanNetworks();
        Serial.printf("  Found %d networks\n", n);
        for (int i = 0; i < n && i < 10; i++) {
          Serial.printf("  %d: %s (%d dBm)\n", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i));
        }
      }
    }

    delay(100);
  } else {
    // Normal mode: deep sleep from setup, shouldn't reach here
    delay(1000);
  }
}

// Sensor reading phase
void run_sensor_phase() {
  PROFILE_SCOPE("run_sensor_phase");
  Serial.println("=== Sensor Phase ===");
  uint32_t phase_start = millis();

  InsideReadings readings = read_sensors_with_timeout(SENSOR_PHASE_TIMEOUT_MS);

  if (isfinite(readings.temperatureC)) {
    Serial.printf("Sensors: %.1f°C, %.1f%% RH, %.1f hPa\n", readings.temperatureC,
                  readings.humidityPct, readings.pressureHPa);

    // Store for publishing
    set_last_published_inside_tempC(readings.temperatureC);
    set_last_published_inside_rh(readings.humidityPct);
    set_last_published_inside_pressureHPa(readings.pressureHPa);
  }

  Serial.printf("Sensor phase took %lu ms\n", millis() - phase_start);
}

// Network and MQTT publishing phase
void run_network_phase() {
  PROFILE_SCOPE("run_network_phase");
  Serial.println("=== Network Phase ===");
  uint32_t phase_start = millis();

  if (!mqtt_is_connected()) {
    Serial.println("MQTT not connected, skipping publish");
    return;
  }

  // Use MQTTBatcher to batch sensor readings for efficient publishing
  MQTTBatcher& batcher = MQTTBatcher::getInstance();
  PubSubClient* client = mqtt_get_client();
  const char* client_id = mqtt_get_client_id();

  // Queue sensor readings
  float tempC = get_last_published_inside_tempC();
  float rhPct = get_last_published_inside_rh();
  float pressHPa = get_last_published_inside_pressureHPa();

  if (isfinite(tempC) && client_id && client_id[0]) {
    // Build topics and payloads, then queue for batched publish
    char topic[64], payload[32];

    // Temperature (Celsius)
    snprintf(topic, sizeof(topic), "espsensor/%s/inside/temperature", client_id);
    snprintf(payload, sizeof(payload), "%.1f", tempC);
    batcher.queue(topic, payload, true);

    // Humidity
    snprintf(topic, sizeof(topic), "espsensor/%s/inside/humidity", client_id);
    snprintf(payload, sizeof(payload), "%.0f", rhPct);
    batcher.queue(topic, payload, true);

    // Pressure
    if (isfinite(pressHPa)) {
      snprintf(topic, sizeof(topic), "espsensor/%s/inside/pressure", client_id);
      snprintf(payload, sizeof(payload), "%.1f", pressHPa);
      batcher.queue(topic, payload, true);
    }
  }

  // Queue battery status
  BatteryStatus bs = read_battery_status();
  if (bs.percent >= 0 && client_id && client_id[0]) {
    char topic[64], payload[32];

    // Battery voltage
    snprintf(topic, sizeof(topic), "espsensor/%s/battery/voltage", client_id);
    snprintf(payload, sizeof(payload), "%.2f", bs.voltage);
    batcher.queue(topic, payload, true);

    // Battery percent
    snprintf(topic, sizeof(topic), "espsensor/%s/battery/percent", client_id);
    snprintf(payload, sizeof(payload), "%d", bs.percent);
    batcher.queue(topic, payload, true);
  }

  // Flush all queued messages in batch
  size_t sent = batcher.flush(client);
  Serial.printf("Batched publish: %u messages sent\n", sent);

  // Publish diagnostics (these are less frequent, publish directly)
  publish_boot_diagnostics();

  // Fetch any retained outside data
  pump_network_ms(FETCH_RETAINED_TIMEOUT_MS);

  Serial.printf("Network phase took %lu ms\n", millis() - phase_start);
}

#if USE_DISPLAY
// Display update phase
// Forward declaration for display update function from display_renderer
extern void full_refresh();

void run_display_phase() {
  PROFILE_SCOPE("run_display_phase");
  Serial.println("=== Display Phase ===");
  uint32_t phase_start = millis();

  // Call the full refresh to update the display with current sensor data
  full_refresh();

  Serial.printf("Display phase took %lu ms\n", millis() - phase_start);
}
#endif

// Deep sleep phase
void run_sleep_phase() {
  Serial.println("=== Sleep Phase ===");

#if ALWAYS_ON
  // Always-on mode never deep sleeps; loop() drives sampling from here on. This
  // is what keeps the device reachable for network OTA.
  Serial.println("ALWAYS_ON: staying awake, sampling from loop()");
  Serial.flush();
  return;
#endif

#if DEV_NO_SLEEP
  Serial.println("DEV_NO_SLEEP: Staying awake in loop()");
  Serial.println("Device will print [ALIVE] message every 5 seconds");
  Serial.flush();
  return;  // Return to setup(), then loop() will run continuously
#endif

  // Calculate adaptive wake interval based on battery level and sensor changes
  SleepConfig config = get_default_sleep_config();
  uint32_t wake_interval_sec = calculate_optimal_sleep_interval(config);

  Serial.printf("Adaptive sleep: %u sec (battery: %d%%, temp changing: %s)\n", wake_interval_sec,
                read_battery_status().percent, is_temperature_changing_rapidly() ? "YES" : "NO");

// Update memory tracking before sleep
#if FEATURE_MEMORY_TRACKING
  MemoryTracker::getInstance().update();
#endif

  // Update cumulative uptime before sleep
  account_uptime();

  // Prepare for sleep
  power_prepare_sleep();
  net_prepare_for_sleep();

  // Store state to NVS
  nvs_end_cache();

#if FEATURE_SD_STORAGE
  // Unmount before the GPIOs go high-Z. Every write already closes its file, so
  // this is not about corruption -- it is about not leaving the card selected and
  // drawing idle current for the whole sleep interval.
  sd_end();
#endif

  Serial.printf("Entering deep sleep for %u seconds\n", wake_interval_sec);
  go_deep_sleep_with_tracking(wake_interval_sec);
}
