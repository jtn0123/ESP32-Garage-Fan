// See web.h. Every handler body is verbatim from the pre-split firmware;
// only the transport (http_tx) and the state broadcast (sse) are indirected.
#include "net/web.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
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
#include "net/origin_check.h"
#include "net/sse.h"
#include "net/web_debug.h"
#include "net/web_ota.h"
#include "net/wifi_link.h"
#include "sensors/battery.h"
#include "sensors/climate.h"
#include "storage/history.h"
#include "storage/sdcard.h"
#include "system/coredump.h"
#include "system/crashlog.h"
#include "system/eventlog.h"
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

// An empty configured token must never authorize anything: with g_token ""
// and no ?token= argument, arg() returns "" and a bare equality check would
// wave the request through. Unreachable with today's defaults, but this is
// the line every privileged route stands behind, so it does not get to rely
// on the defaults staying friendly.
bool token_ok(const String& presented) { return g_token[0] != '\0' && presented == g_token; }

/**
 * Reject a state-changing request that a foreign page told the browser to make.
 *
 * POST-only is NOT sufficient on its own, which is the correction that produced
 * this function: an HTML form can be submitted cross-origin with method=POST
 * and no preflight, so `<form action="http://garage-fan.local/api/set?speed=0">`
 * with a scripted submit() reaches the handler exactly as a GET `<img src>`
 * used to. The response stays unreadable cross-origin, but the write lands, and
 * the write is the whole attack.
 *
 * A request with NO Origin header is allowed: that is curl, the deploy script,
 * and every other non-browser client. Browsers always attach Origin to a
 * cross-origin POST, so this closes the browser-driven path without breaking
 * the bench tooling. These endpoints remain unauthenticated on the LAN by
 * deliberate choice; this is about who gets to speak for the operator.
 */
bool origin_ok() {
  if (!g_http.hasHeader("Origin"))
    return true;  // non-browser caller
  // The comparison itself is net::origin_is_self, which the host tests cover
  // (native_origin_check) -- including the lookalike hosts a prefix match
  // would have let through.
  // Present-but-empty is NOT absent. origin_is_self treats "" as "no browser
  // sent one", which is right for a missing header and wrong here: the client
  // did send it. sse.cpp already drew this distinction; both now agree.
  const String origin = g_http.header("Origin");
  return origin.length() > 0 &&
         net::origin_is_self(origin.c_str(), WiFi.localIP().toString().c_str(), FAN_HOSTNAME);
}

/** origin_ok() with the 403 already sent. True means "keep going". */
bool guard_origin() {
  if (origin_ok())
    return true;
  eventlog::log("web", "rejected cross-origin write from %s", g_http.header("Origin").c_str());
  g_http.send(403, "application/json", "{\"error\":\"cross-origin request refused\"}");
  return false;
}

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
           "\"sd_used_mb\":%lu,\"sd_free_mb\":%lu,"
           "\"batt\":%s,\"rssi\":%d,\"drops\":%lu,\"mqtt\":%s,"
           "\"uptime_s\":%lu,\"ip\":\"%s\"}",
           fan::speed(), fan::auto_on() ? "true" : "false", fan::auto_max(), fan::auto_min(),
           fan::engage_f(), fan::release_f(), outside, climate::offset_active(),
           climate::offset_charging(), climate::offset_idle(), kFwVersion, run ? run->label : "?",
           ota_rollback_image_confirmed() ? "true" : "false",
           (unsigned long)ota_rollback_unhealthy_boots(), climate::ok() ? "true" : "false",
           crashlog::last_death(), (unsigned long)crashlog::boots(), crashlog::prev_death(),
           sdcard::quarantined() ? "true" : "false", (unsigned long)sdcard::total_mb(),
           (unsigned long)sdcard::used_mb(), (unsigned long)sdcard::free_mb(), batt, WiFi.RSSI(),
           (unsigned long)wifi_link::drops(), mqtt_link::connected() ? "true" : "false",
           millis() / 1000UL, WiFi.localIP().toString().c_str());
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

// The last panic, decoded on-device.
//
// Answers the question the flight recorder could not: a panic used to reach us
// as the bare word "panic" because IDF prints the backtrace over a UART this
// board's CDC drops. The dump was always in flash; see system/coredump.h.
static void handle_crash() {
  char buf[768];
  const size_t n = coredump::to_json(buf, sizeof(buf));
  if (n == 0) {
    g_http.send(500, "application/json", "{\"error\":\"could not read the core dump\"}");
    return;
  }
  g_http.send(200, "application/json", buf);
}

// The raw ELF core dump, for scripts/decode_crash.py to turn the backtrace PCs
// into file:line against the matching build.
static void handle_crash_raw() {
  size_t addr = 0, size = 0;
  if (!coredump::image_range(&addr, &size) || size == 0) {
    g_http.send(404, "application/json", "{\"error\":\"no core dump stored\"}");
    return;
  }
  http_tx::Chunked tx(g_http.client(), "application/octet-stream",
                      "Content-Disposition: attachment; filename=coredump.elf\r\n");
  uint8_t chunk[512];
  bool whole = true;
  for (size_t off = 0; off < size && tx.ok(); off += sizeof(chunk)) {
    const size_t want = (size - off) < sizeof(chunk) ? (size - off) : sizeof(chunk);
    if (!coredump::read_image(off, chunk, want)) {
      whole = false;
      break;
    }
    esp_task_wdt_reset();
    tx.write(reinterpret_cast<const char*>(chunk), want);
  }
  // A failed READ used to break out and still call end(), which sends the
  // terminating chunk -- so a short ELF arrived looking complete and decoded
  // to nonsense. Only a body we actually read in full gets finalized. (A failed
  // WRITE already suppresses the terminator through the writer's sticky state.)
  if (whole)
    tx.end();
  else
    tx.abort();
}

// Token-guarded: drop the stored dump so the next panic is unambiguous.
static void handle_crash_erase() {
  if (!guard_origin())
    return;
  if (!token_ok(g_http.arg("token"))) {
    g_http.send(403, "application/json", "{\"error\":\"bad token\"}");
    return;
  }
  const bool ok = coredump::erase();
  eventlog::log("crash", "core dump erased by request (%s)", ok ? "ok" : "failed");
  g_http.send(ok ? 200 : 500, "application/json",
              ok ? "{\"ok\":true}" : "{\"error\":\"erase failed\"}");
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

// The whole stored record, streamed off the card.
//
// This used to serialise the RAM ring, so it handed back only the samples
// since the last reboot -- 308 bytes on the live device -- while the month
// files it was named after sat unreachable on the card. `?days=` selects the
// window; the default of 30 is the full retention the two-month path window
// can address. Falls back to the ring only when there is no card, and says so
// in the filename so the two are never confused.
static void handle_csv() {
  // Same rule as handle_history: a malformed question gets an error, not a
  // plausible answer. This used to clamp silently, so ?days=90 and ?days=abc
  // both returned a 30-day export under a 200 with a filename claiming 30 days.
  int days = 30;
  if (g_http.hasArg("days")) {
    const String raw = g_http.arg("days");
    days = raw.toInt();
    if (days < 1 || days > 30 || String(days) != raw) {
      g_http.send(400, "application/json", "{\"error\":\"days must be 1-30\"}");
      return;
    }
  }

  if (sdcard::ok() && time_synced()) {
    char disp[96];
    snprintf(disp, sizeof(disp), "Content-Disposition: attachment; filename=garage-fan-%dd.csv\r\n",
             days);
    http_tx::Chunked tx(g_http.client(), "text/csv", disp);
    // Header names the 1.14.23 column set; older rows in the same file carry
    // six fields and are streamed exactly as stored rather than back-filled.
    tx.print("epoch,temp_c,rh,hpa,outside_f,speed,batt_v,chg\n");
    sdcard::stream_range(
        time(nullptr) - (time_t)days * 86400,
        [](const char* line, void* ctx) {
          auto* t = static_cast<http_tx::Chunked*>(ctx);
          return t->print(line) && t->print("\n");
        },
        &tx);
    tx.end();
    return;
  }

  const uint16_t n = history::count();
  http_tx::Chunked tx(g_http.client(), "text/csv",
                      "Content-Disposition: attachment; filename=garage-fan-ring.csv\r\n");
  tx.print("epoch,temp_c,rh,hpa,outside_f,speed,batt_v,chg\n");
  for (uint16_t i = 0; i < n && tx.ok(); i++) {
    const long ts =
        history::end_ts() ? (long)history::end_ts() - (long)(n - 1 - i) * (kSampleMs / 1000) : 0;
    tx.printf("%ld,%.2f,%.0f,%.1f,%.1f,%d,%.2f,%d\n", ts, history::temp()[i], history::rh()[i],
              history::hpa()[i], isnan(history::out_f()[i]) ? -999.0f : history::out_f()[i],
              static_cast<int>(history::speed()[i]),
              isnan(history::batt_v()[i]) ? -999.0f : history::batt_v()[i],
              static_cast<int>(history::chg()[i]));
  }
  tx.end();
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
  if (!guard_origin())
    return;

  if (!token_ok(g_http.arg("token"))) {
    g_http.send(403, "application/json", "{\"error\":\"bad token\"}");
    return;
  }
  eventlog::log("web", "restart requested");
  eventlog::flush_tick();  // best effort: land the reason on SD first
  g_http.send(200, "application/json", "{\"ok\":true,\"note\":\"restarting\"}");
  delay(150);  // let the response drain before the reset takes the socket
  esp_restart();
}

static void handle_config() {
  if (!guard_origin())
    return;

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
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"temp_c\":%.2f,\"rh\":%.1f,\"hpa\":%.1f}", t, h, p);
  g_http.send(200, "application/json", buf);
}

// Series writers stream straight to the socket. They used to append to an
// Arduino String whose reserve()/concat() failures nothing checked; with
// per-row timestamps and all seven series the full 288-row body runs past
// 11 KB, and this board's largest contiguous free block has measured 7.7 KB.
// See http_tx::Chunked.
static void write_series(http_tx::Chunked& tx, const char* name, const float* v, uint16_t n,
                         uint8_t decimals) {
  tx.printf("\"%s\":[", name);
  char num[32];
  for (uint16_t i = 0; i < n && tx.ok(); i++) {
    // isfinite, not !isnan: infinity is not valid JSON either, and these
    // values come off the SD card via sscanf with no range check, so a single
    // corrupt row must not tear the whole payload.
    //
    // snprintf, not dtostrf: dtostrf's width argument is a MINIMUM, not a cap,
    // and it always writes the full number. A corrupt row holding 1e38 renders
    // 39 digits plus decimals -- straight past the old 16-byte buffer and into
    // the loop thread's stack.
    if (!isfinite(v[i])) {
      tx.print("null");
    } else {
      const int w = snprintf(num, sizeof(num), "%.*f", static_cast<int>(decimals), v[i]);
      tx.print(w > 0 && w < static_cast<int>(sizeof(num)) ? num : "null");
    }
    if (i + 1 < n)
      tx.print(",");
  }
  tx.print("]");
}

static void write_ints(http_tx::Chunked& tx, const char* name, const int8_t* v, uint16_t n) {
  tx.printf("\"%s\":[", name);
  for (uint16_t i = 0; i < n && tx.ok(); i++) {
    tx.printf(i + 1 < n ? "%d," : "%d", static_cast<int>(v[i]));
  }
  tx.print("]");
}

// Per-row epochs. This is the field whose absence made the long ranges lie:
// with no timestamps the console spaced rows evenly across the requested
// window regardless of when they were actually taken, and could not detect
// an outage because every step measured exactly one nominal interval.
static void write_ts(http_tx::Chunked& tx, const time_t* v, uint16_t n) {
  tx.print("\"ts\":[");
  for (uint16_t i = 0; i < n && tx.ok(); i++) {
    tx.printf(i + 1 < n ? "%ld," : "%ld", static_cast<long>(v[i]));
  }
  tx.print("]");
}

// The ring's rows are uniformly spaced by construction, so its timestamps are
// derived rather than stored -- no scratch array for them.
static void write_ts_derived(http_tx::Chunked& tx, time_t end, uint16_t n, long step) {
  tx.print("\"ts\":[");
  for (uint16_t i = 0; i < n && tx.ok(); i++) {
    const long ts = end ? static_cast<long>(end) - (long)(n - 1 - i) * step : 0;
    tx.printf(i + 1 < n ? "%ld," : "%ld", ts);
  }
  tx.print("]");
}

// Chart scratch for the SD path: 288 rows x 8 columns is ~7.5 KB, which does
// not fit in this build's remaining DRAM (the link failed by 2 KB when it was
// plain BSS). It lives in the 2 MB of PSRAM the board already enables instead
// -- the access penalty is irrelevant for a once-per-range-change request, and
// keeping it out of internal RAM leaves the contiguous block that the SD mount
// and the sockets compete for untouched. Allocated once, on first use.
namespace {
struct GraphScratch {
  time_t* ts = nullptr;
  float* t = nullptr;
  float* h = nullptr;
  float* p = nullptr;
  float* o = nullptr;
  float* b = nullptr;
  int8_t* sp = nullptr;
  int8_t* cg = nullptr;
  bool ready = false;
};
GraphScratch g_scratch;

template <class T>
T* psram_array(uint16_t n) {
  void* m = heap_caps_calloc(n, sizeof(T), MALLOC_CAP_SPIRAM);
  if (!m)  // no PSRAM on this board variant: internal heap is the fallback
    m = heap_caps_calloc(n, sizeof(T), MALLOC_CAP_8BIT);
  return static_cast<T*>(m);
}

bool scratch_ready() {
  if (g_scratch.ready)
    return true;
  g_scratch.ts = psram_array<time_t>(kGraphMaxPts);
  g_scratch.t = psram_array<float>(kGraphMaxPts);
  g_scratch.h = psram_array<float>(kGraphMaxPts);
  g_scratch.p = psram_array<float>(kGraphMaxPts);
  g_scratch.o = psram_array<float>(kGraphMaxPts);
  g_scratch.b = psram_array<float>(kGraphMaxPts);
  g_scratch.sp = psram_array<int8_t>(kGraphMaxPts);
  g_scratch.cg = psram_array<int8_t>(kGraphMaxPts);
  g_scratch.ready = g_scratch.ts && g_scratch.t && g_scratch.h && g_scratch.p && g_scratch.o &&
                    g_scratch.b && g_scratch.sp && g_scratch.cg;
  if (!g_scratch.ready) {
    // Release the blocks that DID succeed. Without this a partial failure
    // leaked them and the next chart request allocated a fresh partial set --
    // under exactly the memory pressure that caused the failure, and
    // scratch_ready() runs on every card-backed request.
    free(g_scratch.ts);
    free(g_scratch.t);
    free(g_scratch.h);
    free(g_scratch.p);
    free(g_scratch.o);
    free(g_scratch.b);
    free(g_scratch.sp);
    free(g_scratch.cg);
    g_scratch = GraphScratch{};
    eventlog::log("web", "graph scratch alloc failed");
  }
  return g_scratch.ready;
}
}  // namespace

static void handle_history() {
  // Reject anything but the documented ?days=1|7|30 instead of quietly
  // serving the RAM ring. A caller who typos the parameter (?range=7d) used
  // to receive a syntactically valid ring response wearing the wrong data --
  // which is how this repo's own SD-read verification fooled itself on
  // 2026-08-10: the LineReader refactor was "confirmed" against a response
  // that never touched the card. Malformed questions get errors, not
  // plausible answers; that rule has now paid for itself three times here
  // (sdformat "ok", /api/sensors "live", and this).
  // The parameter is REQUIRED, not defaulted: a defaulted absent parameter is
  // exactly how ?range=7d (a typo for ?days=7) sailed through and served ring
  // data as if it were the card's. The console always sends days= explicitly.
  const String days_arg = g_http.hasArg("days") ? g_http.arg("days") : "";
  const int days = days_arg.toInt();
  if (days_arg != "1" && days_arg != "7" && days_arg != "30") {
    g_http.send(400, "application/json", "{\"error\":\"days must be 1, 7 or 30\"}");
    return;
  }
  // The card is the record for EVERY range, not just 7 and 30.
  //
  // days=1 used to be hardcoded to the RAM ring, so the 24-hour chart restarted
  // from a single point after each of this device's 40 reboots even though the
  // card held a month of samples. The browser's localStorage merge hid it in
  // one browser and nowhere else; that cache is gone now, and this is why it
  // could go.
  //
  // A range the card cannot answer is an ERROR, not the ring wearing the
  // range's label. Serving 24 h of RAM under a "30 days" request is the same
  // class of confident falsehood as the ?range=7d typo above -- and it was
  // reachable any time the card was unmounted or the clock unsynced.
  const bool have_card = sdcard::ok() && time_synced();
  if (!have_card && days > 1) {
    g_http.send(503, "application/json",
                sdcard::ok() ? "{\"error\":\"clock not synced yet\"}"
                             : "{\"error\":\"sd card not mounted\"}");
    return;
  }

  if (have_card && !scratch_ready()) {
    g_http.send(503, "application/json", "{\"error\":\"out of memory for chart data\"}");
    return;
  }

  const long step = (long)(kSampleMs / 1000);
  http_tx::Chunked tx(g_http.client(), "application/json");
  if (have_card) {
    const GraphScratch& s = g_scratch;
    const sdcard::Samples dst{s.ts, s.t, s.h, s.p, s.o, s.b, s.sp, s.cg};
    const time_t cutoff = time(nullptr) - (time_t)days * 86400;
    const uint16_t n = sdcard::read_range(cutoff, dst, kGraphMaxPts);
    // interval_s is now only a nominal hint for gap detection; ts[] carries
    // the truth. It used to be days*86400/n, which inflated by exactly
    // requested_span/actual_span whenever the card held less than the window.
    tx.printf("{\"source\":\"sd\",\"interval_s\":%ld,", step);
    write_ts(tx, s.ts, n);
    tx.print(",");
    write_series(tx, "temp_c", s.t, n, 1);
    tx.print(",");
    write_series(tx, "rh", s.h, n, 0);
    tx.print(",");
    write_series(tx, "hpa", s.p, n, 1);
    tx.print(",");
    write_series(tx, "out_f", s.o, n, 1);
    tx.print(",");
    write_series(tx, "batt_v", s.b, n, 2);
    tx.print(",");
    write_ints(tx, "spd", s.sp, n);
    tx.print(",");
    write_ints(tx, "chg", s.cg, n);
    tx.print("}");
  } else {
    // No card and days=1: the ring is all there is, and the response says so
    // rather than letting the caller assume persistence it does not have.
    const uint16_t rows = history::count();
    tx.printf("{\"source\":\"ring\",\"interval_s\":%ld,", step);
    write_ts_derived(tx, history::end_ts(), rows, step);
    tx.print(",");
    write_series(tx, "temp_c", history::temp(), rows, 1);
    tx.print(",");
    write_series(tx, "rh", history::rh(), rows, 0);
    tx.print(",");
    write_series(tx, "hpa", history::hpa(), rows, 1);
    tx.print(",");
    write_series(tx, "out_f", history::out_f(), rows, 1);
    tx.print(",");
    write_series(tx, "batt_v", history::batt_v(), rows, 2);
    tx.print(",");
    write_ints(tx, "spd", history::speed(), rows);
    tx.print(",");
    write_ints(tx, "chg", history::chg(), rows);
    tx.print("}");
  }
  tx.end();
}

// Token-guarded one-shot: mount with format-on-failure, turning a fresh
// exFAT card into FAT32 in place. Deliberately NOT automatic on normal
// mounts -- a flaky-but-full card must never be silently erased. Blocks the
// loop for the duration (can be minutes on big cards); the RMT peripheral
// keeps the fan running throughout.
static void handle_sd_format() {
  if (!guard_origin())
    return;

  if (!token_ok(g_http.arg("token"))) {
    g_http.send(403, "application/json", "{\"error\":\"bad token\"}");
    return;
  }
  // "ok" used to mean only "the card mounted afterwards", so this endpoint
  // claimed success for work it had not done. SD.begin's format_if_empty
  // formats an UNMOUNTABLE card only; on a card that already carries a
  // filesystem it is a remount that erases nothing. A 99%-full 28 GB card
  // answered {"ok":true} in 166 ms with every byte still on it (2026-08-09).
  // Report what actually happened, and let the caller see the numbers.
  // used_mb() reads 0 while unmounted -- and an unmountable card is the
  // NORMAL reason to call this endpoint. Without a measurable baseline the
  // erased verdict would be a guess in the dangerous direction ("nothing
  // erased" right after format_if_empty legitimately wrote a fresh FAT32
  // over the card), so the baseline-less case says unknown instead.
  const bool baseline_known = sdcard::ok();
  const uint32_t used_before = sdcard::used_mb();
  const bool mounted = sdcard::format();
  const uint32_t used_after = sdcard::used_mb();
  const bool erased = baseline_known && mounted && used_after < used_before;
  eventlog::log("sd", "format: mounted=%d baseline=%d erased=%d used %lu->%lu MB", mounted ? 1 : 0,
                baseline_known ? 1 : 0, erased ? 1 : 0, (unsigned long)used_before,
                (unsigned long)used_after);
  const char* note = "nothing erased: the card already held a mountable filesystem";
  const char* erased_json = "false";
  if (!baseline_known) {
    note =
        "card was not mounted before the call; whether the filesystem was "
        "rewritten cannot be measured from here";
    erased_json = "null";
  } else if (erased) {
    note = "erased";
    erased_json = "true";
  }
  char buf[352];
  snprintf(buf, sizeof(buf),
           "{\"ok\":%s,\"erased\":%s,\"total_mb\":%lu,\"used_before_mb\":%lu,\"used_mb\":%lu,"
           "\"note\":\"%s\"}",
           mounted ? "true" : "false", erased_json, (unsigned long)sdcard::total_mb(),
           (unsigned long)used_before, (unsigned long)used_after, note);
  g_http.send(mounted ? 200 : 500, "application/json", buf);
}

// Token-guarded: delete the card's CONTENTS, leaving its filesystem alone.
//
// This is the endpoint that actually reclaims space. /api/sdformat cannot:
// SD.begin's format_if_empty only formats a card that fails to mount, so a
// 99%-full but perfectly healthy 28 GB card is simply remounted with every
// byte intact -- which is exactly what the deployed card did, sitting at
// 210 MB free while this firmware's own logs came to 308 bytes.
//
// Destructive and irreversible; the console confirms before calling.
static void handle_sd_purge() {
  if (!guard_origin())
    return;

  if (!token_ok(g_http.arg("token"))) {
    g_http.send(403, "application/json", "{\"error\":\"bad token\"}");
    return;
  }
  if (!sdcard::ok()) {
    g_http.send(503, "application/json", "{\"error\":\"sd card not mounted\"}");
    return;
  }
  const uint32_t free_before = sdcard::free_mb();
  const sdcard::PurgeResult r = sdcard::purge();
  // Three distinct "not finished" cases, and they need different advice. The
  // first real purge reclaimed all 28 GB and then reported "stopped at the
  // entry bound; run again" -- the bound was nowhere near hit (208 entries of
  // 20000); the card simply had the fan's own log back in it by the time the
  // check ran. A queue-bound stop is likewise NOT the entry bound, and saying
  // so pointed the operator at a limit they could not reconcile with what the
  // purge had actually touched.
  char note[192];
  if (r.complete) {
    snprintf(note, sizeof(note), "card contents deleted; filesystem left in place");
  } else if (r.remaining) {
    // Name it. Running again cannot remove a FAT entry that already refused --
    // in practice this is macOS's .Spotlight-V100 stub, which is harmless and
    // occupies nothing, and telling the operator to retry forever is worse
    // than telling them what is actually there.
    snprintf(note, sizeof(note),
             "space reclaimed; %lu entr%s the filesystem will not remove (first: %s)",
             (unsigned long)r.remaining, r.remaining == 1 ? "y remains" : "ies remain", r.leftover);
  } else if (r.too_long) {
    snprintf(note, sizeof(note),
             "%lu entr%s had paths too long to act on safely and were left alone",
             (unsigned long)r.too_long, r.too_long == 1 ? "y" : "ies");
  } else if (r.skipped_dirs) {
    snprintf(note, sizeof(note),
             "%lu director%s did not fit the walk queue; run again to reach them",
             (unsigned long)r.skipped_dirs, r.skipped_dirs == 1 ? "y" : "ies");
  } else {
    snprintf(note, sizeof(note), "stopped at the entry bound; run again to continue");
  }
  char buf[576];
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"files\":%lu,\"dirs\":%lu,\"remaining\":%lu,\"skipped_dirs\":%lu,"
           "\"too_long\":%lu,\"leftover\":\"%s\",\"free_before_mb\":%lu,\"free_mb\":%lu,"
           "\"complete\":%s,\"restarting\":true,\"note\":\"%s\"}",
           (unsigned long)r.files, (unsigned long)r.dirs, (unsigned long)r.remaining,
           (unsigned long)r.skipped_dirs, (unsigned long)r.too_long, r.leftover,
           (unsigned long)free_before, (unsigned long)sdcard::free_mb(),
           r.complete ? "true" : "false", note);
  g_http.send(200, "application/json", buf);

  // Restart after a purge, deliberately.
  //
  // HONEST ACCOUNTING: twice on 2026-08-11 the deployed board panicked roughly
  // 75-100 s AFTER a purge that had already returned success -- long after the
  // handler was done, with nothing on the tape between the purge line and the
  // reboot. The first time was a stack overflow in the old recursive walk and
  // is fixed; the second, on the iterative version, is NOT explained. Mass
  // deletion evidently leaves something in the SD/FATFS layer that faults on a
  // later access.
  //
  // Rebuilding that state from a clean boot removes the window, the same way
  // one reboots after an fsck rather than trusting a repaired filesystem live.
  // It is a workaround, not a diagnosis, and it is written down here as such:
  // if the underlying fault is ever found, this restart should go with it.
  // The fan itself is unaffected -- the RMT peripheral holds its duty across a
  // reset, and the console's own restart button already works this way.
  eventlog::log("sd", "purge done; restarting to rebuild filesystem state");
  eventlog::flush_tick();
  delay(200);
  esp_restart();
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

// The flight recorder. The RAM ring by default (works with no card, this
// boot only); ?sd=1 tails /events.log for the record that survives reboots.
static void handle_events() {
  if (g_http.hasArg("sd")) {
    // 2 KB, static (does not belong on the loop stack): ~20 recent lines of
    // tail is plenty for remote triage, and this board's heap is tight
    // enough that every resident KB fights the SD mount for its slab.
    static char buf[2048];
    const uint32_t n = sdcard::tail_events(buf, sizeof(buf));
    http_tx::send_big(g_http.client(), "text/plain", buf, n);
    return;
  }
  String out;
  char line[104];
  const uint16_t n = eventlog::count();
  out.reserve(static_cast<size_t>(n) * sizeof(line) + 64);
  for (uint16_t i = 0; i < n; i++) {
    eventlog::copy_line(i, line, sizeof(line));
    out += line;
    out += '\n';
  }
  http_tx::send_big(g_http.client(), "text/plain", out.c_str(), out.length());
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
  // POST-only: token-guarded AND destructive; a GET must never reboot the
  // board (link prefetchers and crawlers issue GETs).
  g_http.on("/api/restart", HTTP_POST, handle_restart);
  // POST-only, for the same reason /api/restart and /api/sdformat are: these
  // three CHANGE the device, and a route registered without a method answers
  // GET too. That made every one of them reachable from any web page the
  // operator happened to have open -- `<img src="http://garage-fan.local/
  // api/set?speed=0">` stops the fan, with no auth on the LAN by design -- and
  // reachable by link prefetchers and history-restoring browsers besides. The
  // response is not readable cross-origin (no CORS header here), but the write
  // lands, which is the whole attack.
  g_http.on("/api/set", HTTP_POST, handle_set);
  g_http.on("/api/raw", HTTP_POST, handle_raw);
  g_http.on("/api/config", HTTP_POST, handle_config);
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
  g_http.on("/api/sdformat", HTTP_POST, handle_sd_format);
  g_http.on("/api/sdpurge", HTTP_POST, handle_sd_purge);
  g_http.on("/api/events", handle_events);
  g_http.on("/api/crash", handle_crash);
  g_http.on("/api/crash.bin", handle_crash_raw);
  g_http.on("/api/crash/erase", HTTP_POST, handle_crash_erase);
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
