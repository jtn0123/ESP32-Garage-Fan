// See web.h. The live product surface only: the page, state, device facts,
// config and speed writes, and the live sensor read. The read-side data
// routes live in web_history.cpp, maintenance/forensics in web_maint.cpp,
// bench diagnostics in web_debug.cpp and OTA in web_ota.cpp; the shared
// gates are net/web_gate.h.
#include "net/web.h"

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "esp_ota_ops.h"
#include "fan/control.h"
#include "generated_page.h"
#include "net/http_tx.h"
#include "net/mqtt_link.h"
#include "net/plug.h"
#include "net/sse.h"
#include "net/web_debug.h"
#include "net/web_gate.h"
#include "net/web_history.h"
#include "net/web_maint.h"
#include "net/web_ota.h"
#include "net/wifi_link.h"
#include "sensors/air.h"
#include "sensors/battery.h"
#include "sensors/climate.h"
#include "storage/sdcard.h"
#include "system/crashlog.h"
#include "system/eventlog.h"
#include "system/ota_rollback.h"

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

bool token_ok(const String& presented) { return web_gate::token_ok(g_token, presented); }
bool guard_origin() { return web_gate::guard_origin(g_http); }

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
  // The watt meter on the fan's supply, or null when the poller is disabled
  // or has never read. verdict: 1 agree, -1 disagree, 0 cannot say.
  char plugs[112];
  if (plug::enabled() && plug::age_s() >= 0) {
    char vs[16] = "null";
    if (!isnan(plug::volts()))
      snprintf(vs, sizeof(vs), "%.1f", plug::volts());
    snprintf(plugs, sizeof(plugs), "{\"w\":%.1f,\"v\":%s,\"age_s\":%ld,\"verdict\":%d}",
             plug::watts(), vs, (long)plug::age_s(), plug::verdict());
  } else {
    snprintf(plugs, sizeof(plugs), "null");
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
           "\"sd_used_mb\":%lu,\"sd_free_mb\":%lu,"
           "\"batt\":%s,\"rssi\":%d,\"drops\":%lu,\"mqtt\":%s,"
           "\"uptime_s\":%lu,\"ip\":\"%s\",\"plug\":%s,\"sht_pref\":%s,"
           "\"gas_on\":%s,\"gas_spd\":%d,\"gas_voc\":%d,\"gas_active\":%s}",
           fan::speed(), fan::auto_on() ? "true" : "false", fan::auto_max(), fan::auto_min(),
           fan::engage_f(), fan::release_f(), outside, climate::offset_active(),
           climate::offset_charging(), climate::offset_idle(), kFwVersion, run ? run->label : "?",
           ota_rollback_image_confirmed() ? "true" : "false",
           (unsigned long)ota_rollback_unhealthy_boots(), climate::ok() ? "true" : "false",
           crashlog::last_death(), (unsigned long)crashlog::boots(), crashlog::prev_death(),
           sdcard::quarantined() ? "true" : "false", (unsigned long)sdcard::total_mb(),
           (unsigned long)sdcard::used_mb(), (unsigned long)sdcard::free_mb(), batt, WiFi.RSSI(),
           (unsigned long)wifi_link::drops(), mqtt_link::connected() ? "true" : "false",
           millis() / 1000UL, WiFi.localIP().toString().c_str(), plugs,
           climate::prefer_sht() ? "true" : "false", fan::gas_boost_on() ? "true" : "false",
           fan::gas_speed(), fan::gas_voc_on(), fan::gas_active() ? "true" : "false");
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

static void handle_state() {
  char buf[896];
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

static void handle_config() {
  if (!guard_origin())
    return;

  if (g_http.hasArg("auto"))
    fan::set_auto(g_http.arg("auto").toInt() != 0);
  if (g_http.hasArg("sht"))
    climate::set_prefer_sht(g_http.arg("sht").toInt() != 0);
  if (g_http.hasArg("gason"))
    fan::set_gas_boost(g_http.arg("gason").toInt() != 0);
  if (g_http.hasArg("gasspd")) {
    const int v = g_http.arg("gasspd").toInt();
    if (v >= 1 && v <= 12)
      fan::set_gas_speed(v);
  }
  if (g_http.hasArg("gasvoc")) {
    // Floor of 100: the index recenters on 100 by construction, so a lower
    // trigger would hold the boost latched on ordinary air.
    const int v = g_http.arg("gasvoc").toInt();
    if (v >= 100 && v <= 500)
      fan::set_gas_voc_on(v);
  }
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
  if (g_http.hasArg("newtoken") && token_ok(g_http.arg("auth"))) {
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
  // A LIVE read, not the newest ring entry. History is stored
  // already-corrected -- deliberately, so changing an offset never rewrites
  // what was already logged -- and it is only appended every 5 minutes, so
  // serving it here made a freshly changed offset appear to do nothing at
  // all. Calibrating against a display that will not move is precisely how
  // this board ended up reading ~10 F low: the offset was pushed further and
  // further to chase a number that could not respond until the next sample
  // (2026-08-09). The console polls this once a minute and calls the result
  // `live`; it should be live. Costs one forced BME280 conversion.
  float t;
  float h;
  float p;
  if (!climate::sample(&t, &h, &p)) {
    g_http.send(200, "application/json", "{\"ok\":false}");
    return;
  }
  // The air chain rides along: which source produced temp/rh, and the gas
  // readings with raws -- meaningful from the first second -- beside indices
  // that stay 0 while Sensirion's algorithm warms up. The console labels the
  // warm-up rather than hiding it.
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"temp_c\":%.2f,\"rh\":%.1f,\"hpa\":%.1f,"
           "\"source\":\"%s\",\"voc_raw\":%ld,\"nox_raw\":%ld,\"voc\":%ld,\"nox\":%ld,"
           "\"bme_t\":%.2f,\"bme_rh\":%.1f}",
           t, h, p, climate::sht_driving() ? "sht41" : "bme280", (long)air::voc_raw(),
           (long)air::nox_raw(), (long)air::voc_index(), (long)air::nox_index(),
           climate::bme_temp_c(), climate::bme_rh());
  g_http.send(200, "application/json", buf);
}

// Calibration instrument: drive an arbitrary duty, no reflash per data point.
static void handle_raw() {
  if (!guard_origin())
    return;

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
  if (!guard_origin())
    return;

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

}  // namespace

void begin(Preferences* prefs) {
  g_prefs = prefs;
  String tk = prefs ? prefs->getString("token", FAN_OTA_TOKEN) : String(FAN_OTA_TOKEN);
  snprintf(g_token, sizeof(g_token), "%s", tk.c_str());
  // WebServer discards every header it was not told to keep, so guard_origin()
  // would see nothing -- and, since "no Origin" means "non-browser caller",
  // would wave every cross-origin write straight through. This one line is
  // what makes the CSRF guard real rather than decorative.
  {
    const char* kWanted[] = {"Origin"};
    g_http.collectHeaders(kWanted, 1);
  }
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
  // POST-only: these three CHANGE the device, and a route registered without
  // a method answers GET too. That made every one of them reachable from any
  // web page the operator happened to have open -- `<img src="http://
  // garage-fan.local/api/set?speed=0">` stops the fan, with no auth on the
  // LAN by design -- and reachable by link prefetchers and history-restoring
  // browsers besides. The response is not readable cross-origin (no CORS
  // header here), but the write lands, which is the whole attack.
  g_http.on("/api/set", HTTP_POST, handle_set);
  g_http.on("/api/raw", HTTP_POST, handle_raw);
  g_http.on("/api/config", HTTP_POST, handle_config);
  g_http.on("/api/sensors", handle_sensors);
  g_http.on("/manifest.json", []() {
    http_tx::send_big(g_http.client(), "application/json", kManifest, sizeof(kManifest) - 1);
  });
  g_http.on("/icon.svg", []() {
    http_tx::send_big(g_http.client(), "image/svg+xml", kIcon, sizeof(kIcon) - 1);
  });
  web_history::register_routes(g_http);
  web_maint::register_routes(g_http, g_token);
  web_debug::register_routes(g_http, g_token);
  web_ota::register_routes(g_http, g_token);
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
