// Garage fan controller: the ESP32 replaces the iLiving wall controller,
// replaying its PWM protocol (docs/fan_protocol/PROTOCOL.md; polarity
// corrected against the real fan 2026-08-04) through a BSS138 shifter.
//
// Control:  web UI http://garage-fan.local/ · /api/set?speed=0..12 ·
//           MQTT garage/fan/set · /api/raw?high_pct= (calibration)
// Auto:     differential thermostat vs outdoors (fan_auto_logic.h, natively
//           tested). Outdoor temp arrives on MQTT_SUB_BASE "/temp_f" from the
//           existing home/outdoor feed. Hotter outside -> min speed; hotter
//           inside -> ramp toward the user's max. Manual set disables auto.
// Climate:  BME280 samples every 5 min -> 24 h RAM ring + CSV on the microSD
//           card (monthly files, epoch-stamped once SNTP syncs) -> web graph
//           at 24 h / 7 d / 30 d ranges.
// Persist:  speed + auto config in NVS (restored before WiFi), commands also
//           retained on the broker; whichever answers first wins the tie.
// OTA:      POST /update?token=...; A/B slots with ota_rollback confirm.
#include <Adafruit_BME280.h>
#include <Adafruit_EPD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_LC709203F.h>
#include <Adafruit_MAX1704X.h>
#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <SD.h>
#include <SPI.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>

#include <ctime>

#include "esp_ota_ops.h"
#include "lwip/sockets.h"
#include "esp_task_wdt.h"
#include "config.h"
#include "system/crashlog.h"
#include "sensors/battery.h"
#include "fan/auto_logic.h"
#include "generated_page.h"
#include "system/ota_rollback.h"

static WiFiClient g_net;
static PubSubClient g_mqtt(g_net);
static WebServer g_http(80);
static WiFiServer g_sse_srv(8081);
static WiFiClient g_sse[4];
// 2.13" mono SSD1680 FeatherWing, landscape; no busy/rst pins wired.
static Adafruit_SSD1680 g_epd(250, 122, EPD_DC_PIN, -1, EPD_CS_PIN, -1, -1);
static Adafruit_BME280 g_bme;
static Preferences g_prefs;
static bool g_bme_ok = false;
static bool g_sd_ok = false;
static uint8_t g_sd_fails = 0;  // give up retrying after 10; format resets
static bool g_sd_quarantined = false;
static char g_last_death[48] = "none";
static char g_prev_death[48] = "none";  // the boot before this one, from NVS
static uint32_t g_boots = 0;            // lifetime boot count, NVS

static bool g_rmt_ready = false;
static bool g_services_up = false;
static bool g_ever_healthy = false;
static bool g_ota_authorized = false;
static bool g_auto_on = false;
static int g_auto_max = 9;
static rmt_data_t g_wave;
static int g_speed = 0;
static float g_inside_c = NAN;
static float g_outside_c = NAN;
static uint32_t g_outside_ms = 0;
static time_t g_outdoor_epoch = 0;  // bridge's own timestamp topic, if any
// Board self-heating correction: charging current warms the board-mounted
// BME280 far more than idle operation does. User-tunable via
// /api/config?offc=&offi= (Celsius, added to the raw reading).
static float g_off_chg = -3.0f;
static float g_off_idle = -1.0f;
static uint32_t g_last_reconnect_ms = 0;
static uint32_t g_mqtt_up_ms = 0;
static uint32_t g_last_sample_ms = 0;
static uint32_t g_last_auto_ms = 0;
static float g_ring_t[kRingLen], g_ring_h[kRingLen], g_ring_p[kRingLen];
static float g_ring_o[kRingLen];   // outdoor F at sample time (NAN = none)
static int8_t g_ring_s[kRingLen];  // fan speed at sample time
static float g_ring_bv[kRingLen];  // battery volts at sample time (NAN = none)
static int8_t g_ring_c[kRingLen];  // charging verdict (1/0, -1 = no battery)
static time_t g_ring_end_ts = 0;
static uint16_t g_ring_count = 0;
// Runtime odometer + energy estimate (cubic fan law, ~105 W flat out).
static uint32_t g_run_total_s = 0, g_run_today_s = 0;
static uint32_t g_today_ymd = 0;
static float g_energy_wh = 0;
static uint32_t g_last_count_ms = 0;
static uint32_t g_last_nvs_ms = 0;
static bool g_epd_ok = false;
static uint32_t g_last_epd_ms = 0;
static int g_epd_speed_shown = -99;
static char g_token[40];
static int g_auto_min = 0;                           // rest speed once equalized (0 = off)
static float g_auto_onf = 2.5f, g_auto_offf = 1.5f;  // engage/release, deg F
static bool g_auto_high = false;                     // hysteresis latch for fan_auto_decide

static void set_wave(uint16_t high_us) {
  if (!g_rmt_ready) {
    if (!rmtInit(FAN_PWM_PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 1000000)) {
      Serial.println("rmtInit FAILED");
      return;
    }
    g_rmt_ready = true;
  }
  if (high_us >= kPeriodUs || high_us == 0) {
    g_wave.level0 = high_us ? 1 : 0;
    g_wave.duration0 = kPeriodUs / 2;
    g_wave.level1 = high_us ? 1 : 0;
    g_wave.duration1 = kPeriodUs - kPeriodUs / 2;
  } else {
    g_wave.level0 = 1;
    g_wave.duration0 = high_us;
    g_wave.level1 = 0;
    g_wave.duration1 = kPeriodUs - high_us;
  }
  rmtWriteLooping(FAN_PWM_PIN, &g_wave, 1);
}

static float fan_watts(int speed) {
  if (speed <= 0)
    return 0;
  const float f = speed / 12.0f;
  return 5.0f + 100.0f * f * f * f;  // cubic fan law, rough estimate
}

static void sse_push();

static void publish_state() {
  if (!g_mqtt.connected())
    return;
  char buf[4];
  snprintf(buf, sizeof(buf), "%d", g_speed);
  g_mqtt.publish(kTopicState, buf, true);
}

// source: "boot", "http", "mqtt", "auto". Manual sources disable auto mode
// (the human explicitly grabbed the wheel); retained-replay right after the
// broker connects does not count as manual.
static void apply_speed(int v, const char* source) {
  if (v < 0 || v > 12 || v == g_speed)
    return;
  const bool manual = strcmp(source, "http") == 0 ||
                      (strcmp(source, "mqtt") == 0 && millis() - g_mqtt_up_ms > kMqttGraceMs);
  if (manual && g_auto_on) {
    g_auto_on = false;
    g_prefs.putBool("auto", false);
    Serial.println("auto mode off (manual override)");
  }
  g_speed = v;
  set_wave(kHighUs[v]);
  publish_state();
  if (strcmp(source, "mqtt") != 0 && g_mqtt.connected()) {
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", v);
    g_mqtt.publish(kTopicSet, buf, true);
  }
  g_prefs.putInt("speed", v);  // survives power loss even broker-less
  sse_push();
  Serial.printf("speed -> %d via %s\n", v, source);
}

static bool time_synced();

static bool is_charging() { return battery::charging(); }

static float temp_corrected(float raw) { return raw + (is_charging() ? g_off_chg : g_off_idle); }

static float outside_c_fresh() {
  // A bridge that publishes its own epoch timestamp gives real freshness --
  // retained redelivery of an old snapshot reads as stale, as it should.
  // Feeds without a ts topic fall back to receipt-time freshness.
  if (g_outdoor_epoch > 0) {
    if (!time_synced() || time(nullptr) - g_outdoor_epoch > 1800)
      return NAN;
    return g_outside_c;
  }
  if (g_outside_ms == 0 || millis() - g_outside_ms > kOutdoorStaleMs)
    return NAN;
  return g_outside_c;
}

static void auto_tick() {
  if (!g_auto_on)
    return;
  if (g_bme_ok) {
    const float t = g_bme.readTemperature();
    if (!isnan(t))
      g_inside_c = temp_corrected(t);
  }
  FanAutoCfg cfg = kFanAutoDefaults;
  cfg.min_speed = g_auto_min;
  cfg.max_speed = g_auto_max;
  cfg.on_delta_c = g_auto_onf * 5 / 9;  // user thinks in F; logic runs in C
  cfg.off_delta_c = g_auto_offf * 5 / 9;
  const int next =
      fan_auto_decide(g_inside_c, outside_c_fresh(), g_speed < 0 ? 0 : g_speed, &g_auto_high, cfg);
  if (next != g_speed)
    apply_speed(next, "auto");
}

static bool time_synced() { return time(nullptr) > 1700000000; }

static void sd_mount() {
  // Full teardown between attempts, per storage.cpp: a card interrupted
  // mid-transaction by a reset stops answering until the bus restarts from
  // silence. Called at boot and retried from loop() until a card appears.
  static const uint32_t kFreqs[] = {4000000, 1000000, 400000};
  for (size_t i = 0; i < 3; i++) {
    if (i > 0) {
      SD.end();
      SPI.end();
      delay(50);
      SPI.begin();
    }
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
    pinMode(SRAM_CS_PIN, OUTPUT);
    digitalWrite(SRAM_CS_PIN, HIGH);
    pinMode(EPD_CS_PIN, OUTPUT);
    digitalWrite(EPD_CS_PIN, HIGH);
    if (SD.begin(SD_CS_PIN, SPI, kFreqs[i])) {
      g_sd_ok = true;
      Serial.printf("sd mounted at %lu Hz: %.1f/%.1f MB used\n", (unsigned long)kFreqs[i],
                    SD.usedBytes() / 1048576.0, SD.totalBytes() / 1048576.0);
      return;
    }
  }
}

static void sd_mount_guarded() {
  crashlog::rtc_sd_sentinel = crashlog::kSdSentinelMagic;
  CRUMB("sd_mount");
  sd_mount();
  CRUMB_CLEAR();
  crashlog::rtc_sd_sentinel = 0;
}

static void sd_log_sample(time_t now, float t, float h, float p) {
  if (!g_sd_ok)
    return;
  struct tm tm_now;
  gmtime_r(&now, &tm_now);
  char path[36];  // worst-case int expansion of the two %d fields, not 4+2
  snprintf(path, sizeof(path), "/climate-%04d%02d.csv", tm_now.tm_year + 1900, tm_now.tm_mon + 1);
  CRUMB("sd_write");
  File f = SD.open(path, FILE_APPEND);
  if (!f) {
    CRUMB_CLEAR();
    g_sd_ok = false;  // card yanked; re-detect on reboot
    return;
  }
  const float out = outside_c_fresh();
  char line[96];
  int n = snprintf(line, sizeof(line), "%ld,%.2f,%.1f,%.1f,%.2f,%d\n", (long)now, t, h, p,
                   isnan(out) ? -999.0f : out, g_speed);
  f.write((const uint8_t*)line, n);
  f.close();
  CRUMB_CLEAR();
}

static void sample_climate() {
  if (!g_bme_ok) {
    g_bme_ok = g_bme.begin(0x77, &Wire) || g_bme.begin(0x76, &Wire);
    if (!g_bme_ok)
      return;
    Serial.println("bme280 detected");
  }
  const float t_raw = g_bme.readTemperature();
  const float h = g_bme.readHumidity();
  const float p = g_bme.readPressure() / 100.0f;
  if (isnan(t_raw) || isnan(h) || isnan(p)) {
    g_bme_ok = false;
    return;
  }
  const float t = temp_corrected(t_raw);
  g_inside_c = t;
  // Battery first so this sample's ring row records the fresh reading and
  // the charging verdict it produced, not the 5-minute-old one.
  battery::sample();
  if (g_ring_count == kRingLen) {
    memmove(g_ring_t, g_ring_t + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_ring_h, g_ring_h + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_ring_p, g_ring_p + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_ring_o, g_ring_o + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_ring_s, g_ring_s + 1, (kRingLen - 1) * sizeof(int8_t));
    memmove(g_ring_bv, g_ring_bv + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_ring_c, g_ring_c + 1, (kRingLen - 1) * sizeof(int8_t));
    g_ring_count--;
  }
  g_ring_t[g_ring_count] = t;
  g_ring_h[g_ring_count] = h;
  g_ring_p[g_ring_count] = p;
  const float oc = outside_c_fresh();
  g_ring_o[g_ring_count] = isnan(oc) ? NAN : oc * 9 / 5 + 32;
  g_ring_s[g_ring_count] = (int8_t)(g_speed < 0 ? 0 : g_speed);
  g_ring_bv[g_ring_count] = battery::kind() ? battery::volts() : NAN;
  g_ring_c[g_ring_count] = battery::kind() ? (battery::charging() ? 1 : 0) : -1;
  g_ring_count++;
  if (time_synced())
    g_ring_end_ts = time(nullptr);
  if (time_synced())
    sd_log_sample(time(nullptr), t, h, p);
  if (g_mqtt.connected()) {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"temp_c\":%.2f,\"rh\":%.1f,\"hpa\":%.1f}", t, h, p);
    g_mqtt.publish(kTopicClimate, buf, true);
  }
}

static void state_json(char* out, size_t cap) {
  const esp_partition_t* run = esp_ota_get_running_partition();
  const float oc = outside_c_fresh();
  char outside[16];
  if (isnan(oc))
    snprintf(outside, sizeof(outside), "null");
  else
    snprintf(outside, sizeof(outside), "%.1f", oc * 9 / 5 + 32);
  bool chg = false;
  float eta = NAN;
  battery::eta(&chg, &eta);
  char batt[96];
  if (battery::kind() && !isnan(battery::volts())) {
    char mvh[16] = "null";
    const float slope = battery::slope_mv_per_h();
    if (!isnan(slope))
      snprintf(mvh, sizeof(mvh), "%.0f", slope);
    char etas[16] = "null";
    if (!isnan(eta))
      snprintf(etas, sizeof(etas), "%.1f", eta);
    char pcts[16] = "null";
    if (!isnan(battery::percent()))
      snprintf(pcts, sizeof(pcts), "%.0f", battery::percent());
    snprintf(batt, sizeof(batt), "{\"v\":%.3f,\"pct\":%s,\"chg\":%s,\"eta_h\":%s,\"mvh\":%s}",
             battery::volts(), pcts, chg ? "true" : "false", etas, mvh);
  } else {
    snprintf(batt, sizeof(batt), "null");
  }
  snprintf(out, cap,
           "{\"speed\":%d,\"auto\":%s,\"auto_max\":%d,\"auto_min\":%d,"
           "\"on_f\":%.1f,\"off_f\":%.1f,\"outside_f\":%s,"
           "\"toff\":%.1f,\"offc\":%.1f,\"offi\":%.1f,"
           "\"fw\":\"%s\",\"slot\":\"%s\",\"confirmed\":%s,"
           "\"unhealthy_boots\":%lu,\"sensor\":%s,\"last_reset\":\"%s\","
           "\"boots\":%lu,\"prev_death\":\"%s\","
           "\"sd_q\":%s,"
           "\"sd_total_mb\":%lu,"
           "\"sd_used_mb\":%lu,\"batt\":%s,\"rssi\":%d,\"mqtt\":%s,"
           "\"uptime_s\":%lu,\"ip\":\"%s\"}",
           g_speed, g_auto_on ? "true" : "false", g_auto_max, g_auto_min, g_auto_onf, g_auto_offf,
           outside, is_charging() ? g_off_chg : g_off_idle, g_off_chg, g_off_idle, kFwVersion,
           run ? run->label : "?", ota_rollback_image_confirmed() ? "true" : "false",
           (unsigned long)ota_rollback_unhealthy_boots(), g_bme_ok ? "true" : "false", g_last_death,
           (unsigned long)g_boots, g_prev_death, g_sd_quarantined ? "true" : "false",
           g_sd_ok ? (unsigned long)(SD.totalBytes() / 1048576) : 0,
           g_sd_ok ? (unsigned long)(SD.usedBytes() / 1048576) : 0, batt, WiFi.RSSI(),
           g_mqtt.connected() ? "true" : "false", millis() / 1000UL,
           WiFi.localIP().toString().c_str());
}

// Never block on an SSE peer. NetworkClient::write retries a 1 s select up
// to 10 times per call, so one sleeping laptop with a full socket buffer
// costs 10 s per print -- sse_push's three prints hit the 30 s task
// watchdog and panic the board (proven live 2026-08-05: crash loop after
// v1.11.0 with several dashboards open). Instead: zero-timeout select +
// MSG_DONTWAIT send, and any peer that can't take the whole frame right
// now is dropped -- the browser's EventSource auto-reconnects.
static void sse_send(WiFiClient& c, const char* buf, int n) {
  const int s = c.fd();
  if (s < 0) {
    c.stop();
    return;
  }
  fd_set set;
  timeval tv{0, 0};
  FD_ZERO(&set);
  FD_SET(s, &set);
  if (select(s + 1, nullptr, &set, nullptr, &tv) <= 0 || !FD_ISSET(s, &set)) {
    c.stop();
    return;
  }
  if (send(s, buf, n, MSG_DONTWAIT) != n)
    c.stop();
}

static void sse_push() {
  char st[768];
  char frame[832];
  state_json(st, sizeof(st));
  const int n = snprintf(frame, sizeof(frame), "data: %s\n\n", st);
  for (auto& c : g_sse) {
    if (c && c.connected())
      sse_send(c, frame, n);
  }
}

static void sse_accept() {
  WiFiClient nc = g_sse_srv.accept();
  if (!nc)
    return;
  for (auto& c : g_sse) {
    if (!c || !c.connected()) {
      c = nc;
      c.setNoDelay(true);
      char st[768];
      char frame[1024];
      state_json(st, sizeof(st));
      const int n = snprintf(frame, sizeof(frame),
                             "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                             "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n"
                             "Connection: keep-alive\r\n\r\nretry: 3000\n\ndata: %s\n\n",
                             st);
      sse_send(c, frame, n);
      return;
    }
  }
  nc.stop();  // table full
}

static void epd_render() {
  if (!g_epd_ok)
    return;
  CRUMB("epd");
  g_epd.clearBuffer();
  g_epd.fillScreen(EPD_WHITE);
  g_epd.setTextColor(EPD_BLACK);
  const float tin = g_ring_count ? g_ring_t[g_ring_count - 1] : NAN;
  g_epd.setTextSize(4);
  g_epd.setCursor(4, 18);
  if (isnan(tin))
    g_epd.print("--.-");
  else
    g_epd.printf("%.1f", tin * 9 / 5 + 32);
  g_epd.setTextSize(1);
  g_epd.setCursor(6, 54);
  g_epd.print("GARAGE F");
  const float rh = g_ring_count ? g_ring_h[g_ring_count - 1] : NAN;
  g_epd.setCursor(70, 54);
  if (!isnan(rh))
    g_epd.printf("RH %.0f%%", rh);
  g_epd.setTextSize(3);
  g_epd.setCursor(158, 8);
  if (g_speed <= 0)
    g_epd.print("OFF");
  else
    g_epd.printf("F%d", g_speed);
  g_epd.setTextSize(1);
  g_epd.setCursor(158, 38);
  g_epd.print(g_auto_on ? "AUTO ON" : "AUTO OFF");
  const float oc = outside_c_fresh();
  g_epd.setCursor(158, 52);
  if (isnan(oc))
    g_epd.print("OUT --");
  else
    g_epd.printf("OUT %.1fF", oc * 9 / 5 + 32);
  g_epd.setCursor(158, 66);
  if (!isnan(battery::volts()))
    g_epd.printf("BAT %.2fV %.0f%%", battery::volts(),
                 isnan(battery::percent()) ? 0 : battery::percent());
  g_epd.setCursor(6, 76);
  g_epd.printf("run today %luh%02lum  total %luh", (unsigned long)(g_run_today_s / 3600),
               (unsigned long)(g_run_today_s % 3600 / 60), (unsigned long)(g_run_total_s / 3600));
  g_epd.setCursor(6, 106);
  g_epd.printf("%s  fw %s", WiFi.localIP().toString().c_str(), kFwVersion);
  g_epd.display();
  CRUMB_CLEAR();
  g_last_epd_ms = millis();
  g_epd_speed_shown = g_speed;
}

static void handle_stats() {
  float tmin = NAN, tmax = NAN, tsum = 0;
  for (uint16_t i = 0; i < g_ring_count; i++) {
    const float t = g_ring_t[i];
    if (isnan(tmin) || t < tmin)
      tmin = t;
    if (isnan(tmax) || t > tmax)
      tmax = t;
    tsum += t;
  }
  char buf[288];
  snprintf(buf, sizeof(buf),
           "{\"run_today_s\":%lu,\"run_total_s\":%lu,\"energy_wh\":%.0f,"
           "\"watts_now\":%.0f,\"t_min_f\":%.1f,\"t_max_f\":%.1f,"
           "\"t_avg_f\":%.1f,\"samples\":%u}",
           (unsigned long)g_run_today_s, (unsigned long)g_run_total_s, g_energy_wh,
           fan_watts(g_speed < 0 ? 0 : g_speed), isnan(tmin) ? 0 : tmin * 9 / 5 + 32,
           isnan(tmax) ? 0 : tmax * 9 / 5 + 32,
           g_ring_count ? (tsum / g_ring_count) * 9 / 5 + 32 : 0, g_ring_count);
  g_http.send(200, "application/json", buf);
}

// Bounded raw send: WebServer's own writes go through NetworkClient::write,
// which burns up to 10 s of 1 s selects PER CALL against a peer that stops
// draining. The 21.8 KB page goes out in ~16 chunks, so one stalled browser
// meant 160 s of stall -- the 30 s task watchdog panicked mid-serve (the
// user-visible "clipped" truncated pages) and the board crash-looped. This
// path never blocks more than 50 ms at a time, feeds the watchdog between
// slices, and drops any peer that makes no progress for 4 s.
static bool send_bounded(int s, const char* p, size_t n) {
  uint32_t last_progress = millis();
  size_t off = 0;
  while (off < n) {
    fd_set set;
    timeval tv{0, 50000};
    FD_ZERO(&set);
    FD_SET(s, &set);
    const int r = select(s + 1, nullptr, &set, nullptr, &tv);
    esp_task_wdt_reset();
    if (r < 0)
      return false;
    if (r > 0 && FD_ISSET(s, &set)) {
      const int w = send(s, p + off, n - off, MSG_DONTWAIT);
      if (w > 0) {
        off += w;
        last_progress = millis();
        continue;
      }
      if (w < 0 && errno != EAGAIN)
        return false;
    }
    if (millis() - last_progress > 4000)
      return false;  // peer stalled: drop it, never stall loop()
  }
  return true;
}

static void send_big(const char* mime, const char* body, size_t len, const char* extra_hdr = "") {
  WiFiClient c = g_http.client();
  const int s = c.fd();
  if (s < 0)
    return;
  char hdr[224];
  const int hn = snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                          "%sConnection: close\r\n\r\n",
                          mime, (unsigned)len, extra_hdr);
  if (!send_bounded(s, hdr, hn) || !send_bounded(s, body, len)) {
  }
  c.stop();  // Connection: close either way; a stalled peer is already gone
}

static void handle_csv() {
  String out;
  out.reserve(g_ring_count * 44 + 64);
  out += "epoch,temp_c,rh,hpa,outside_f,speed\n";
  for (uint16_t i = 0; i < g_ring_count; i++) {
    char l[80];
    const long ts = g_ring_end_ts ? (long)g_ring_end_ts - (long)(g_ring_count - 1 - i) * 300 : 0;
    snprintf(l, sizeof(l), "%ld,%.2f,%.0f,%.1f,%.1f,%d\n", ts, g_ring_t[i], g_ring_h[i],
             g_ring_p[i], isnan(g_ring_o[i]) ? -999 : g_ring_o[i], (int)g_ring_s[i]);
    out += l;
  }
  send_big("text/csv", out.c_str(), out.length(),
           "Content-Disposition: attachment; filename=garage-fan-24h.csv\r\n");
}

static const char kManifest[] PROGMEM = R"json({
"name":"Garage fan","short_name":"GarageFan","start_url":"/",
"display":"standalone","background_color":"#0b0e13","theme_color":"#0b0e13",
"icons":[{"src":"/icon.svg","sizes":"any","type":"image/svg+xml"}]})json";

static const char kIcon[] PROGMEM =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<rect width="100" height="100" rx="22" fill="#0b0e13"/>
<g fill="#3b82f6"><circle cx="50" cy="50" r="9"/>
<path d="M50 14a10 10 0 0 1 10 10c0 8-6 12-8 18l-4-1c-2-9-8-13-8-19a10 10 0 0 1 10-8z"/>
<path d="M50 86a10 10 0 0 1-10-10c0-8 6-12 8-18l4 1c2 9 8 13 8 19a10 10 0 0 1-10 8z"/>
<path d="M14 50a10 10 0 0 1 10-10c8 0 12 6 18 8l-1 4c-9 2-13 8-19 8a10 10 0 0 1-8-10z"/>
<path d="M86 50a10 10 0 0 1-10 10c-8 0-12-6-18-8l1-4c9-2 13-8 19-8a10 10 0 0 1 8 10z"/>
</g></svg>)svg";

static void handle_state() {
  char buf[768];
  state_json(buf, sizeof(buf));
  g_http.send(200, "application/json", buf);
}

// Copy src into a JSON string body, escaping the two characters that would
// otherwise terminate it early. The SSID and broker host come from the user's
// .env, so one stray quote there would take out the whole settings pane.
static void json_str(char* dst, size_t cap, const char* src) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 2 < cap; i++) {
    if (src[i] == '"' || src[i] == '\\')
      dst[j++] = '\\';
    dst[j++] = src[i];
  }
  dst[j] = 0;
}

// Facts the console needs exactly once per page load: the measured duty table
// -- the single source of truth behind the PWM readouts, the scope trace and
// the capture grid -- plus identity and network strings that never change at
// runtime. Deliberately not folded into /api/state, which is polled every 15 s
// and pushed on every SSE frame.
static void handle_device() {
  char high[96];
  size_t n = 0;
  for (size_t i = 0; i < sizeof(kHighUs) / sizeof(kHighUs[0]) && n < sizeof(high); i++)
    n += snprintf(high + n, sizeof(high) - n, i ? ",%u" : "%u", kHighUs[i]);
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char ssid[80], host[80];
  json_str(ssid, sizeof(ssid), WIFI_SSID);
  json_str(host, sizeof(host), MQTT_HOST);
  char out[512];
  snprintf(out, sizeof(out),
           "{\"id\":\"%s-%02x%02x%02x\",\"host\":\"%s\",\"repo\":\"%s\","
           "\"broker\":\"%s:%d\",\"ssid\":\"%s\",\"topic_set\":\"%s\","
           "\"topic_out\":\"%s\",\"period_us\":%u,\"sample_s\":%lu,\"high_us\":[%s]}",
           FAN_HOSTNAME, mac[3], mac[4], mac[5], FAN_HOSTNAME, FAN_GITHUB_REPO, host, MQTT_PORT,
           ssid, kTopicSet, kTopicOutdoor, (unsigned)kPeriodUs, (unsigned long)(kSampleMs / 1000),
           high);
  g_http.send(200, "application/json", out);
}

// Token-guarded reboot behind the console's maintenance row. Shares the OTA
// token so reaching the page is not by itself enough to bounce the fan.
static void handle_restart() {
  if (g_http.arg("token") != g_token) {
    g_http.send(403, "application/json", "{\"error\":\"bad token\"}");
    return;
  }
  g_http.send(200, "application/json", "{\"ok\":true,\"note\":\"restarting\"}");
  delay(150);  // let the response drain before the reset takes the socket
  esp_restart();
}

static void handle_config() {
  if (g_http.hasArg("auto")) {
    g_auto_on = g_http.arg("auto").toInt() != 0;
    g_prefs.putBool("auto", g_auto_on);
    Serial.printf("auto mode %s\n", g_auto_on ? "on" : "off");
  }
  if (g_http.hasArg("max")) {
    const int m = g_http.arg("max").toInt();
    if (m >= 1 && m <= 12) {
      g_auto_max = m;
      g_prefs.putInt("max", m);
    }
  }
  if (g_http.hasArg("offc")) {
    const float v = g_http.arg("offc").toFloat();
    if (v >= -15 && v <= 15) {
      g_off_chg = v;
      g_prefs.putFloat("offc", v);
    }
  }
  if (g_http.hasArg("offi")) {
    const float v = g_http.arg("offi").toFloat();
    if (v >= -15 && v <= 15) {
      g_off_idle = v;
      g_prefs.putFloat("offi", v);
    }
  }
  if (g_http.hasArg("min")) {
    const int m = g_http.arg("min").toInt();
    if (m >= 0 && m <= 12) {
      g_auto_min = m;
      g_prefs.putInt("amin", m);
    }
  }
  if (g_http.hasArg("onf")) {
    const float v = g_http.arg("onf").toFloat();
    if (v >= 0.5f && v <= 20) {
      g_auto_onf = v;
      g_prefs.putFloat("onf", v);
    }
  }
  if (g_http.hasArg("offf")) {
    const float v = g_http.arg("offf").toFloat();
    if (v >= 0 && v <= 20) {
      g_auto_offf = v;
      g_prefs.putFloat("offf", v);
    }
  }
  // Hysteresis needs release strictly below engage or the latch flaps.
  if (g_auto_offf >= g_auto_onf) {
    g_auto_offf = g_auto_onf > 0.5f ? g_auto_onf - 0.5f : 0;
    g_prefs.putFloat("offf", g_auto_offf);
  }
  if (g_http.hasArg("newtoken") && g_http.arg("auth") == g_token) {
    const String nt = g_http.arg("newtoken");
    if (nt.length() >= 6 && nt.length() < 39) {
      snprintf(g_token, sizeof(g_token), "%s", nt.c_str());
      g_prefs.putString("token", g_token);
      Serial.println("ota token changed");
    }
  }
  sse_push();
  handle_state();
}

static void handle_sensors() {
  if (!g_bme_ok || g_ring_count == 0) {
    g_http.send(200, "application/json", "{\"ok\":false}");
    return;
  }
  char buf[128];
  const uint16_t i = g_ring_count - 1;
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"temp_c\":%.2f,\"rh\":%.1f,\"hpa\":%.1f}", g_ring_t[i],
           g_ring_h[i], g_ring_p[i]);
  g_http.send(200, "application/json", buf);
}

static void append_series(String& out, const char* name, const float* v, uint16_t n,
                          uint8_t decimals) {
  out += '"';
  out += name;
  out += "\":[";
  char num[16];
  for (uint16_t i = 0; i < n; i++) {
    if (isnan(v[i])) {
      // A literal "nan" is not JSON -- one bad sample would make the
      // browser's parser reject the entire history payload (blank graph).
      out += "null";
    } else {
      dtostrf(v[i], 0, decimals, num);
      out += num;
    }
    if (i + 1 < n)
      out += ',';
  }
  out += ']';
}

// Stream the month files covering [cutoff, now], decimating by stride into
// the caller's arrays. Two passes: count, then collect every (count/max)th.
static uint16_t sd_read_range(time_t cutoff, float* t, float* h, float* p, uint16_t max_pts) {
  CRUMB("sd_read");
  time_t now = time(nullptr);
  char paths[2][36];
  int npaths = 0;
  for (time_t at = cutoff; npaths < 2; at = now) {
    struct tm tmv;
    gmtime_r(&at, &tmv);
    char path[36];
    snprintf(path, sizeof(path), "/climate-%04d%02d.csv", tmv.tm_year + 1900, tmv.tm_mon + 1);
    if (npaths == 0 || strcmp(path, paths[0]) != 0) {
      snprintf(paths[npaths], sizeof(paths[npaths]), "%s", path);
      npaths++;
    }
    if (at == now)
      break;
  }
  uint32_t rows = 0;
  for (int pass = 0; pass < 2; pass++) {
    const uint32_t stride = pass ? (rows > max_pts ? rows / max_pts + 1 : 1) : 1;
    uint32_t seen = 0;
    uint16_t kept = 0;
    for (int i = 0; i < npaths; i++) {
      File f = SD.open(paths[i], FILE_READ);
      if (!f)
        continue;
      // Line-buffered scan; lines are short and epoch-prefixed.
      char line[96];
      size_t ll = 0;
      while (f.available()) {
        const char c = (char)f.read();
        if (c != '\n') {
          if (ll < sizeof(line) - 1)
            line[ll++] = c;
          continue;
        }
        line[ll] = '\0';
        ll = 0;
        const long epoch = strtol(line, nullptr, 10);
        if (epoch < cutoff)
          continue;
        if (pass == 0) {
          rows++;
          continue;
        }
        if (seen++ % stride)
          continue;
        if (kept >= max_pts)
          break;
        float tv, hv, pv;
        if (sscanf(line, "%*d,%f,%f,%f", &tv, &hv, &pv) == 3) {
          t[kept] = tv;
          h[kept] = hv;
          p[kept] = pv;
          kept++;
        }
      }
      f.close();
    }
    if (pass == 1) {
      CRUMB_CLEAR();
      return kept;
    }
    if (rows == 0) {
      CRUMB_CLEAR();
      return 0;
    }
  }
  CRUMB_CLEAR();
  return 0;
}

static void handle_history() {
  const int days = g_http.hasArg("days") ? g_http.arg("days").toInt() : 1;
  String out;
  out.reserve(13000);
  if (days <= 1 || !g_sd_ok || !time_synced()) {
    char hd[64];
    snprintf(hd, sizeof(hd), "{\"interval_s\":300,\"end_ts\":%ld,", (long)g_ring_end_ts);
    out += hd;
    append_series(out, "temp_c", g_ring_t, g_ring_count, 1);
    out += ',';
    append_series(out, "rh", g_ring_h, g_ring_count, 0);
    out += ',';
    append_series(out, "hpa", g_ring_p, g_ring_count, 1);
    out += ',';
    append_series(out, "out_f", g_ring_o, g_ring_count, 1);
    out += ',';
    append_series(out, "batt_v", g_ring_bv, g_ring_count, 2);
    out += ",\"spd\":[";
    for (uint16_t i = 0; i < g_ring_count; i++) {
      char n[6];
      snprintf(n, sizeof(n), "%d", (int)g_ring_s[i]);
      out += n;
      if (i + 1 < g_ring_count)
        out += ',';
    }
    out += "],\"chg\":[";
    for (uint16_t i = 0; i < g_ring_count; i++) {
      char n[6];
      snprintf(n, sizeof(n), "%d", (int)g_ring_c[i]);
      out += n;
      if (i + 1 < g_ring_count)
        out += ',';
    }
    out += "]}";
  } else {
    static float t[kGraphMaxPts], h[kGraphMaxPts], p[kGraphMaxPts];
    const time_t cutoff = time(nullptr) - (time_t)days * 86400;
    const uint16_t n = sd_read_range(cutoff, t, h, p, kGraphMaxPts);
    char head[48];
    snprintf(head, sizeof(head), "{\"interval_s\":%ld,", n > 1 ? (long)(days * 86400L / n) : 300L);
    out += head;
    append_series(out, "temp_c", t, n, 1);
    out += ',';
    append_series(out, "rh", h, n, 0);
    out += ',';
    append_series(out, "hpa", p, n, 1);
    out += '}';
  }
  send_big("application/json", out.c_str(), out.length());
}

// Token-guarded one-shot: mount with format-on-failure, turning a fresh
// exFAT card into FAT32 in place. Deliberately NOT automatic on normal
// mounts -- a flaky-but-full card must never be silently erased. Blocks the
// loop for the duration (can be minutes on big cards); the RMT peripheral
// keeps the fan running throughout.
static void handle_sd_format() {
  if (g_http.arg("token") != g_token) {
    g_http.send(403, "application/json", "{\"error\":\"bad token\"}");
    return;
  }
  Serial.println("[SD] format requested");
  esp_task_wdt_delete(NULL);  // a big-card format legitimately takes minutes
  g_sd_quarantined = false;   // manual override un-quarantines
  crashlog::rtc_sd_sentinel = crashlog::kSdSentinelMagic;
  CRUMB("sd_format");
  SD.end();
  SPI.end();
  delay(50);
  SPI.begin();
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  pinMode(SRAM_CS_PIN, OUTPUT);
  digitalWrite(SRAM_CS_PIN, HIGH);
  pinMode(EPD_CS_PIN, OUTPUT);
  digitalWrite(EPD_CS_PIN, HIGH);
  const bool ok = SD.begin(SD_CS_PIN, SPI, 4000000, "/sd", 5, true);
  crashlog::rtc_sd_sentinel = 0;
  CRUMB_CLEAR();
  esp_task_wdt_add(NULL);
  g_sd_ok = ok;
  g_sd_fails = 0;
  char buf[80];
  snprintf(buf, sizeof(buf), "{\"ok\":%s,\"total_mb\":%lu}", ok ? "true" : "false",
           ok ? (unsigned long)(SD.totalBytes() / 1048576) : 0);
  Serial.printf("[SD] format result: %s\n", buf);
  g_http.send(ok ? 200 : 500, "application/json", buf);
}

// Raw SD probe: bit-level CMD0/CMD8 handshake at 400 kHz, reporting each
// step, so "card not seated", "card dead", and "card incompatible" stop
// looking identical. Read-only; safe on any card.
static void handle_sd_test() {
  SD.end();
  SPI.end();
  delay(50);
  SPI.begin();
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  pinMode(SRAM_CS_PIN, OUTPUT);
  digitalWrite(SRAM_CS_PIN, HIGH);
  pinMode(EPD_CS_PIN, OUTPUT);
  digitalWrite(EPD_CS_PIN, HIGH);
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 10; i++) SPI.transfer(0xFF);  // 80 warm-up clocks
  digitalWrite(SD_CS_PIN, LOW);
  static const uint8_t kCmd0[] = {0x40, 0, 0, 0, 0, 0x95};
  for (uint8_t b : kCmd0) SPI.transfer(b);
  uint8_t r1 = 0xFF;
  for (int i = 0; i < 16 && (r1 & 0x80); i++) r1 = SPI.transfer(0xFF);
  uint8_t r7[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t r1b = 0xFF;
  if (r1 == 0x01) {  // idle: try CMD8 (SDv2 voltage check, echoes 0x1AA)
    static const uint8_t kCmd8[] = {0x48, 0, 0, 0x01, 0xAA, 0x87};
    for (uint8_t b : kCmd8) SPI.transfer(b);
    for (int i = 0; i < 16 && (r1b & 0x80); i++) r1b = SPI.transfer(0xFF);
    if (r1b == 0x01)
      for (int i = 0; i < 4; i++) r7[i] = SPI.transfer(0xFF);
  }
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.transfer(0xFF);
  SPI.endTransaction();
  char buf[160];
  snprintf(buf, sizeof(buf),
           "{\"cmd0_r1\":\"0x%02X\",\"cmd8_r1\":\"0x%02X\","
           "\"cmd8_echo\":\"%02X%02X%02X%02X\",\"verdict\":\"%s\"}",
           r1, r1b, r7[0], r7[1], r7[2], r7[3],
           r1 == 0xFF   ? "no response - card absent, unseated, or bad contact"
           : r1 == 0x01 ? (r7[2] == 0x01 && r7[3] == 0xAA ? "card alive, SDv2, handshake ok"
                                                          : "card alive but CMD8 odd")
                        : "card answered abnormally");
  Serial.printf("[SD] probe: %s\n", buf);
  g_http.send(200, "application/json", buf);
}

// Calibration instrument: drive an arbitrary duty, no reflash per data point.
static void handle_raw() {
  if (!g_http.hasArg("high_pct")) {
    g_http.send(400, "application/json", "{\"error\":\"high_pct 0-100\"}");
    return;
  }
  const int pct = g_http.arg("high_pct").toInt();
  if (pct < 0 || pct > 100) {
    g_http.send(400, "application/json", "{\"error\":\"high_pct 0-100\"}");
    return;
  }
  set_wave((uint16_t)((uint32_t)kPeriodUs * pct / 100));
  g_speed = -1;
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"raw_high_pct\":%d}", pct);
  g_http.send(200, "application/json", buf);
}

static void handle_set() {
  if (!g_http.hasArg("speed")) {
    g_http.send(400, "application/json", "{\"error\":\"speed required\"}");
    return;
  }
  const int v = g_http.arg("speed").toInt();
  if (v < 0 || v > 12) {
    g_http.send(400, "application/json", "{\"error\":\"0-12 only\"}");
    return;
  }
  if (g_speed == -1 && v == 0)
    g_speed = -2;  // force off to re-apply after raw
  apply_speed(v, "http");
  handle_state();
}

static void handle_update_upload() {
  HTTPUpload& up = g_http.upload();
  if (up.status == UPLOAD_FILE_START) {
    g_ota_authorized = g_http.arg("token") == g_token;
    if (!g_ota_authorized) {
      Serial.println("[OTA] rejected: bad token");
      return;
    }
    Serial.printf("[OTA] receiving %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
      Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE && g_ota_authorized) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize)
      Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END && g_ota_authorized) {
    if (Update.end(true))
      Serial.printf("[OTA] received %u bytes ok\n", up.totalSize);
    else
      Update.printError(Serial);
  }
}

static void handle_update_done() {
  if (!g_ota_authorized) {
    g_http.send(403, "application/json", "{\"error\":\"bad token\"}");
    return;
  }
  if (Update.hasError()) {
    g_http.send(500, "application/json", "{\"error\":\"update failed\"}");
    return;
  }
  g_http.send(200, "application/json", "{\"ok\":true,\"note\":\"rebooting into new slot\"}");
  delay(300);
  esp_restart();
}

static void on_message(char* topic, uint8_t* payload, unsigned int len) {
  char buf[16];
  if (len >= sizeof(buf))
    return;
  memcpy(buf, payload, len);
  buf[len] = '\0';
  if (strstr(topic, "/ts") != nullptr &&
      strncmp(topic, MQTT_SUB_BASE, strlen(MQTT_SUB_BASE)) == 0) {
    const long e = strtol(buf, nullptr, 10);
    if (e > 1700000000)
      g_outdoor_epoch = e;
    return;
  }
  if (strcmp(topic, kTopicOutdoor) == 0) {
    char* end = nullptr;
    const float f = strtof(buf, &end);
    if (end != buf && isfinite(f) && f > -60 && f < 150) {
      g_outside_c = (f - 32.0f) * 5.0f / 9.0f;
      g_outside_ms = millis();
    }
    return;
  }
  if (strcmp(topic, kTopicSet) != 0 || len == 0 || len > 2)
    return;
  char* end = nullptr;
  long v = strtol(buf, &end, 10);
  if (end == buf || *end != '\0')
    return;
  apply_speed(static_cast<int>(v), "mqtt");
  publish_state();
}

static void ensure_mqtt() {
  if (WiFi.status() != WL_CONNECTED)
    return;
  if (g_mqtt.connected() || millis() - g_last_reconnect_ms < 3000)
    return;
  g_last_reconnect_ms = millis();
  char id[24];
  snprintf(id, sizeof(id), "garage-fan-%06llx", ESP.getEfuseMac() & 0xffffff);
  if (g_mqtt.connect(id, MQTT_USER, MQTT_PASS, kTopicAvail, 0, true, "offline")) {
    Serial.println("mqtt connected");
    g_mqtt_up_ms = millis();
    g_ever_healthy = true;
    ota_rollback_mark_healthy();
    g_mqtt.publish(kTopicAvail, "online", true);
    publish_state();
    g_mqtt.subscribe(kTopicSet);
    g_mqtt.subscribe(kTopicOutdoor);
    g_mqtt.subscribe(MQTT_SUB_BASE "/ts");
  } else {
    Serial.printf("mqtt connect failed rc=%d\n", g_mqtt.state());
  }
}

void setup() {
  pinMode(FAN_PWM_PIN, OUTPUT);
  digitalWrite(FAN_PWM_PIN, LOW);  // off on this rig = line low
  pinMode(FAN_SPARE_PIN, OUTPUT);
  digitalWrite(FAN_SPARE_PIN, HIGH);
  Serial.begin(115200);
  ota_rollback_check_at_boot();
  g_prefs.begin("fanctl", false);
  g_auto_on = g_prefs.getBool("auto", false);
  g_auto_max = g_prefs.getInt("max", 9);
  g_off_chg = g_prefs.getFloat("offc", -3.0f);
  g_off_idle = g_prefs.getFloat("offi", -1.0f);
  g_auto_min = g_prefs.getInt("amin", 0);
  g_auto_onf = g_prefs.getFloat("onf", 2.5f);
  g_auto_offf = g_prefs.getFloat("offf", 1.5f);
  battery::restore(&g_prefs);
  g_run_total_s = g_prefs.getUInt("runs", 0);
  g_run_today_s = g_prefs.getUInt("runt", 0);
  g_today_ymd = g_prefs.getUInt("ymd", 0);
  g_energy_wh = g_prefs.getFloat("ewh", 0);
  String tk = g_prefs.getString("token", FAN_OTA_TOKEN);
  snprintf(g_token, sizeof(g_token), "%s", tk.c_str());
  const int saved = g_prefs.getInt("speed", 0);
  if (saved > 0 && saved <= 12) {
    g_speed = saved;
    set_wave(kHighUs[saved]);  // resume before WiFi even exists
    Serial.printf("restored speed %d from nvs\n", saved);
  }
  // SD stays OUT of the boot path: the controller comes fully online first,
  // and the first mount attempt happens ~60 s later from loop(). A card that
  // crashes the mount gets quarantined by the sentinel above -- no card can
  // take fan control down with it.
  const esp_reset_reason_t rr = esp_reset_reason();
  const bool abnormal = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
                        rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT;
  snprintf(g_last_death, sizeof(g_last_death), "%s%s%s", crashlog::reset_reason_str(rr),
           crashlog::rtc_crumb[0] ? " during " : "",
           crashlog::rtc_crumb[0] ? crashlog::rtc_crumb : "");
  // Quarantine on a mount sentinel from any reset, or any abnormal death
  // while an SD op was in flight.
  g_sd_quarantined = (crashlog::rtc_sd_sentinel == crashlog::kSdSentinelMagic) ||
                     (abnormal && strncmp(crashlog::rtc_crumb, "sd", 2) == 0);
  crashlog::rtc_sd_sentinel = 0;
  CRUMB_CLEAR();
  if (g_sd_quarantined)
    Serial.println("[SD] previous boot died in an SD op -- card quarantined");
  Serial.printf("last reset: %s\n", g_last_death);
  // Longitudinal forensics: RTC evidence dies with power, NVS doesn't. Keep
  // the prior boot's certificate and a lifetime boot counter so a reboot
  // storm is visible from /api/state even after the fact.
  {
    String pd = g_prefs.getString("pdeath", "none");
    snprintf(g_prev_death, sizeof(g_prev_death), "%s", pd.c_str());
    g_prefs.putString("pdeath", g_last_death);
    g_boots = g_prefs.getUInt("boots", 0) + 1;
    g_prefs.putUInt("boots", g_boots);
  }
  // Task watchdog: a frozen loop (hung bus op, wedged driver) force-reboots
  // in 30 s instead of hanging dark forever. The fan rides through on RMT.
  // 60 s: tolerates worst-case framework writes to one stalled peer (10 s
  // per NetworkClient::write call) on the small JSON endpoints while still
  // catching genuine hangs. Large payloads use send_bounded and never stall.
  esp_task_wdt_config_t wdt_cfg = {.timeout_ms = 60000, .idle_core_mask = 0, .trigger_panic = true};
  if (esp_task_wdt_init(&wdt_cfg) != ESP_OK)
    esp_task_wdt_reconfigure(&wdt_cfg);
  esp_task_wdt_add(NULL);
  Wire.begin();
  SPI.begin();
  CRUMB("epd_init");
  g_epd.begin();
  g_epd.setRotation(1);
  g_epd_ok = true;
  CRUMB_CLEAR();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(FAN_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org");
  g_mqtt.setServer(MQTT_HOST, MQTT_PORT);
  g_mqtt.setCallback(on_message);
  g_http.on("/", []() {
    // Unconditionally gzipped: the page is only ever stored compressed, and
    // there is no browser in service that does not accept it. A CLI client
    // wanting to read it needs curl --compressed.
    send_big("text/html", reinterpret_cast<const char*>(kPageGz), sizeof(kPageGz),
             "Content-Encoding: gzip\r\n");
  });
  g_http.on("/api/state", handle_state);
  g_http.on("/api/device", handle_device);
  g_http.on("/api/restart", handle_restart);
  g_http.on("/api/set", handle_set);
  g_http.on("/api/raw", handle_raw);
  g_http.on("/api/config", handle_config);
  g_http.on("/api/sensors", handle_sensors);
  g_http.on("/api/history", handle_history);
  g_http.on("/api/stats", handle_stats);
  g_http.on("/download.csv", handle_csv);
  g_http.on("/manifest.json",
            []() { send_big("application/json", kManifest, sizeof(kManifest) - 1); });
  g_http.on("/icon.svg", []() { send_big("image/svg+xml", kIcon, sizeof(kIcon) - 1); });
  g_http.on("/api/sdformat", handle_sd_format);
  g_http.on("/api/battdebug", []() {
    char out[512];
    const bool w15 = battery::write_reg(0x15, 0x0001);
    const bool w0b = battery::write_reg(0x0B, 0x0056);
    String r;
    for (int variant = 0; variant < 2; variant++) {
      for (uint8_t reg : {(uint8_t)0x09, (uint8_t)0x0D, (uint8_t)0x11}) {
        Wire.beginTransmission(0x0B);
        Wire.write(reg);
        const int wtx = Wire.endTransmission(variant == 1);
        int got = 0;
        uint8_t raw[3] = {0, 0, 0};
        if (wtx == 0) {
          got = Wire.requestFrom((uint8_t)0x0B, (uint8_t)3);
          for (int i = 0; i < got && i < 3; i++) raw[i] = Wire.read();
        }
        const uint8_t chk[5] = {0x16, reg, 0x17, raw[0], raw[1]};
        char e[96];
        snprintf(e, sizeof(e),
                 "{\"reg\":\"0x%02X\",\"stop\":%d,\"wtx\":%d,\"got\":%d,"
                 "\"bytes\":\"%02X%02X%02X\",\"val\":%u,\"crc_ok\":%s},",
                 reg, variant, wtx, got, raw[0], raw[1], raw[2],
                 (unsigned)(((uint16_t)raw[1] << 8) | raw[0]),
                 battery::crc8(chk, 5) == raw[2] ? "true" : "false");
        r += e;
      }
    }
    snprintf(out, sizeof(out), "{\"w15\":%s,\"w0b\":%s,\"reads\":[", w15 ? "true" : "false",
             w0b ? "true" : "false");
    String full = String(out) + r.substring(0, r.length() - 1) + "]}";
    g_http.send(200, "application/json", full);
  });
  g_http.on("/api/i2cscan", []() {
    String out = "{\"found\":[";
    bool first = true;
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        if (!first)
          out += ',';
        char b[8];
        snprintf(b, sizeof(b), "\"0x%02X\"", a);
        out += b;
        first = false;
      }
    }
    out += "]}";
    g_http.send(200, "application/json", out);
  });
  g_http.on("/api/sdtest", handle_sd_test);
  g_http.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
  g_http.onNotFound([]() { g_http.send(404, "application/json", "{\"error\":\"404\"}"); });
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
        g_http.begin();
        g_sse_srv.begin();
        Serial.printf("web ui: http://%s.local/\n", FAN_HOSTNAME);
      }
    }
  }
  esp_task_wdt_reset();
  ensure_mqtt();
  g_mqtt.loop();
  g_http.handleClient();
  sse_accept();
  // Runtime odometer + energy integration, once per second.
  if (millis() - g_last_count_ms >= 1000) {
    g_last_count_ms = millis();
    if (g_speed > 0) {
      g_run_total_s++;
      g_run_today_s++;
      g_energy_wh += fan_watts(g_speed) / 3600.0f;
    }
    if (time_synced()) {
      struct tm lt;
      time_t now = time(nullptr);
      gmtime_r(&now, &lt);
      const uint32_t ymd = (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
      if (g_today_ymd != 0 && ymd != g_today_ymd)
        g_run_today_s = 0;
      g_today_ymd = ymd;
    }
  }
  if (millis() - g_last_nvs_ms >= 15 * 60 * 1000) {
    g_last_nvs_ms = millis();
    g_prefs.putUInt("runs", g_run_total_s);
    g_prefs.putUInt("runt", g_run_today_s);
    g_prefs.putUInt("ymd", g_today_ymd);
    g_prefs.putFloat("ewh", g_energy_wh);
  }
  // E-ink: refresh with each sample cycle, or 60 s after a speed change.
  if (g_epd_ok && g_ring_count &&
      ((millis() - g_last_epd_ms >= kSampleMs) ||
       (g_speed != g_epd_speed_shown && millis() - g_last_epd_ms >= 60000)))
    epd_render();
  if (g_last_sample_ms == 0 || millis() - g_last_sample_ms >= kSampleMs) {
    g_last_sample_ms = millis();
    sample_climate();
    sse_push();
  }
  if (millis() - g_last_auto_ms >= kAutoTickMs) {
    g_last_auto_ms = millis();
    auto_tick();
    battery::begin();
    battery::read();
    // First mount attempt ~60 s after boot, then every 5 min, 10-fail cap,
    // and never while quarantined; /api/sdformat overrides all of it.
    static bool sd_tried = false;
    static uint8_t sd_backoff = 0;
    if (!g_sd_ok && !g_sd_quarantined && g_sd_fails < 10) {
      const bool due = sd_tried ? (++sd_backoff >= 10) : (millis() > 60000);
      if (due) {
        sd_tried = true;
        sd_backoff = 0;
        sd_mount_guarded();
        g_sd_fails = g_sd_ok ? 0 : g_sd_fails + 1;
      }
    }
  }
  if (!g_ever_healthy && !ota_rollback_image_confirmed() && millis() > kUnconfirmedDeadlineMs) {
    Serial.println("[OTA] unconfirmed image never reached broker; restarting");
    Serial.flush();
    esp_restart();
  }
  delay(2);
}
