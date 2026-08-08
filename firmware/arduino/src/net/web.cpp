// See web.h. Every handler body is verbatim from the pre-split firmware;
// only the transport (http_tx) and the state broadcast (sse) are indirected.
#include "net/web.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "config.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "fan/control.h"
#include "generated_page.h"
#include "net/http_tx.h"
#include "net/mqtt_link.h"
#include "net/sse.h"
#include "net/web_debug.h"
#include "sensors/battery.h"
#include "sensors/climate.h"
#include "storage/history.h"
#include "storage/sdcard.h"
#include "system/crashlog.h"
#include "system/odometer.h"
#include "system/ota_rollback.h"
#include "system/timeutil.h"

// Static assets served verbatim. At file scope (not in the namespace) so the
// unindented raw-string bodies do not fight the namespace indent rule --
// their bytes are the response and cannot be re-indented.
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

namespace web {
namespace {

WebServer g_http(80);
Preferences* g_prefs = nullptr;
char g_token[40];
bool g_ota_authorized = false;

}  // namespace

void state_json(char* out, size_t cap) {
  const esp_partition_t* run = esp_ota_get_running_partition();
  const float oc = climate::outside_c_fresh();
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
           fan::speed(), fan::auto_on() ? "true" : "false", fan::auto_max(), fan::auto_min(),
           fan::engage_f(), fan::release_f(), outside, climate::offset_active(),
           climate::offset_charging(), climate::offset_idle(), kFwVersion, run ? run->label : "?",
           ota_rollback_image_confirmed() ? "true" : "false",
           (unsigned long)ota_rollback_unhealthy_boots(), climate::ok() ? "true" : "false",
           crashlog::last_death(), (unsigned long)crashlog::boots(), crashlog::prev_death(),
           sdcard::quarantined() ? "true" : "false", (unsigned long)sdcard::total_mb(),
           (unsigned long)sdcard::used_mb(), batt, WiFi.RSSI(),
           mqtt_link::connected() ? "true" : "false", millis() / 1000UL,
           WiFi.localIP().toString().c_str());
}

void push_state() { sse::push(); }

namespace {

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

static void handle_stats() {
  float tmin, tmax, tavg;
  history::temp_stats(&tmin, &tmax, &tavg);
  char buf[288];
  snprintf(buf, sizeof(buf),
           "{\"run_today_s\":%lu,\"run_total_s\":%lu,\"energy_wh\":%.0f,"
           "\"watts_now\":%.0f,\"t_min_f\":%.1f,\"t_max_f\":%.1f,"
           "\"t_avg_f\":%.1f,\"samples\":%u}",
           (unsigned long)odometer::run_today_s(), (unsigned long)odometer::run_total_s(),
           odometer::energy_wh(), fan::watts(fan::speed() < 0 ? 0 : fan::speed()),
           isnan(tmin) ? 0 : tmin * 9 / 5 + 32, isnan(tmax) ? 0 : tmax * 9 / 5 + 32,
           isnan(tavg) ? 0 : tavg * 9 / 5 + 32, history::count());
  g_http.send(200, "application/json", buf);
}

static void handle_csv() {
  const uint16_t n = history::count();
  String out;
  out.reserve(n * 44 + 64);
  out += "epoch,temp_c,rh,hpa,outside_f,speed\n";
  for (uint16_t i = 0; i < n; i++) {
    char l[80];
    const long ts = history::end_ts() ? (long)history::end_ts() - (long)(n - 1 - i) * 300 : 0;
    snprintf(l, sizeof(l), "%ld,%.2f,%.0f,%.1f,%.1f,%d\n", ts, history::temp()[i], history::rh()[i],
             history::hpa()[i], isnan(history::out_f()[i]) ? -999 : history::out_f()[i],
             static_cast<int>(history::speed()[i]));
    out += l;
  }
  http_tx::send_big(g_http.client(), "text/csv", out.c_str(), out.length(),
                    "Content-Disposition: attachment; filename=garage-fan-24h.csv\r\n");
}

static void handle_state() {
  char buf[768];
  state_json(buf, sizeof(buf));
  g_http.send(200, "application/json", buf);
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
  if (g_http.hasArg("auto"))
    fan::set_auto(g_http.arg("auto").toInt() != 0);
  if (g_http.hasArg("max")) {
    const int m = g_http.arg("max").toInt();
    if (m >= 1 && m <= 12)
      fan::set_auto_max(m);
  }
  if (g_http.hasArg("offc")) {
    const float v = g_http.arg("offc").toFloat();
    if (v >= -15 && v <= 15)
      climate::set_offset_charging(v);
  }
  if (g_http.hasArg("offi")) {
    const float v = g_http.arg("offi").toFloat();
    if (v >= -15 && v <= 15)
      climate::set_offset_idle(v);
  }
  if (g_http.hasArg("min")) {
    const int m = g_http.arg("min").toInt();
    if (m >= 0 && m <= 12)
      fan::set_auto_min(m);
  }
  if (g_http.hasArg("onf")) {
    const float v = g_http.arg("onf").toFloat();
    if (v >= 0.5f && v <= 20)
      fan::set_engage_f(v);
  }
  if (g_http.hasArg("offf")) {
    const float v = g_http.arg("offf").toFloat();
    if (v >= 0 && v <= 20)
      fan::set_release_f(v);
  }
  fan::enforce_hysteresis_gap();
  if (g_http.hasArg("newtoken") && g_http.arg("auth") == g_token) {
    const String nt = g_http.arg("newtoken");
    if (nt.length() >= 6 && nt.length() < 39) {
      snprintf(g_token, sizeof(g_token), "%s", nt.c_str());
      if (g_prefs)
        g_prefs->putString("token", g_token);
      Serial.println("ota token changed");
    }
  }
  push_state();
  handle_state();
}

static void handle_sensors() {
  if (!climate::ok() || history::count() == 0) {
    g_http.send(200, "application/json", "{\"ok\":false}");
    return;
  }
  char buf[128];
  const uint16_t i = history::count() - 1;
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"temp_c\":%.2f,\"rh\":%.1f,\"hpa\":%.1f}",
           history::temp()[i], history::rh()[i], history::hpa()[i]);
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

static void handle_history() {
  const int days = g_http.hasArg("days") ? g_http.arg("days").toInt() : 1;
  String out;
  out.reserve(13000);
  if (days <= 1 || !sdcard::ok() || !time_synced()) {
    const uint16_t rows = history::count();
    char hd[64];
    snprintf(hd, sizeof(hd), "{\"interval_s\":300,\"end_ts\":%ld,", (long)history::end_ts());
    out += hd;
    append_series(out, "temp_c", history::temp(), rows, 1);
    out += ',';
    append_series(out, "rh", history::rh(), rows, 0);
    out += ',';
    append_series(out, "hpa", history::hpa(), rows, 1);
    out += ',';
    append_series(out, "out_f", history::out_f(), rows, 1);
    out += ',';
    append_series(out, "batt_v", history::batt_v(), rows, 2);
    out += ",\"spd\":[";
    for (uint16_t i = 0; i < rows; i++) {
      char n[6];
      snprintf(n, sizeof(n), "%d", static_cast<int>(history::speed()[i]));
      out += n;
      if (i + 1 < rows)
        out += ',';
    }
    out += "],\"chg\":[";
    for (uint16_t i = 0; i < rows; i++) {
      char n[6];
      snprintf(n, sizeof(n), "%d", static_cast<int>(history::chg()[i]));
      out += n;
      if (i + 1 < rows)
        out += ',';
    }
    out += "]}";
  } else {
    static float t[kGraphMaxPts], h[kGraphMaxPts], p[kGraphMaxPts];
    const time_t cutoff = time(nullptr) - (time_t)days * 86400;
    const uint16_t n = sdcard::read_range(cutoff, t, h, p, kGraphMaxPts);
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
  http_tx::send_big(g_http.client(), "application/json", out.c_str(), out.length());
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
  const bool ok = sdcard::format();
  char buf[80];
  snprintf(buf, sizeof(buf), "{\"ok\":%s,\"total_mb\":%lu}", ok ? "true" : "false",
           (unsigned long)sdcard::total_mb());
  Serial.printf("[SD] format result: %s\n", buf);
  g_http.send(ok ? 200 : 500, "application/json", buf);
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
  fan::raw_high_us(static_cast<uint16_t>(static_cast<uint32_t>(kPeriodUs) * pct / 100));
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
  fan::apply(v, "http", /*manual=*/true);
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

}  // namespace

void begin(Preferences* prefs) {
  g_prefs = prefs;
  String tk = prefs ? prefs->getString("token", FAN_OTA_TOKEN) : String(FAN_OTA_TOKEN);
  snprintf(g_token, sizeof(g_token), "%s", tk.c_str());
  sse::set_state_source(state_json);
  g_http.on("/", []() {
    // Unconditionally gzipped: the page is only ever stored compressed, and
    // there is no browser in service that does not accept it. A CLI client
    // wanting to read it needs curl --compressed.
    http_tx::send_big(g_http.client(), "text/html", reinterpret_cast<const char*>(kPageGz),
                      sizeof(kPageGz), "Content-Encoding: gzip\r\n");
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
  g_http.on("/manifest.json", []() {
    http_tx::send_big(g_http.client(), "application/json", kManifest, sizeof(kManifest) - 1);
  });
  g_http.on("/icon.svg", []() {
    http_tx::send_big(g_http.client(), "image/svg+xml", kIcon, sizeof(kIcon) - 1);
  });
  g_http.on("/api/sdformat", handle_sd_format);
  web_debug::register_routes(g_http);
  g_http.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
  g_http.onNotFound([]() { g_http.send(404, "application/json", "{\"error\":\"404\"}"); });
}

void start() {
  g_http.begin();
  sse::start();
}

void handle() {
  g_http.handleClient();
  sse::accept();
}

}  // namespace web
