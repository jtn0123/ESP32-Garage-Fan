// See web_history.h. Handler bodies verbatim from web.cpp at the split;
// only the WebServer reference is indirected.
#include "net/web_history.h"

#include <Arduino.h>

#include <cmath>
#include <cstdio>
#include <ctime>

#include "config.h"
#include "esp_heap_caps.h"
#include "fan/control.h"
#include "generated_wire.h"
#include "net/http_tx.h"
#include "storage/bootlog.h"
#include "storage/history.h"
#include "storage/sdcard.h"
#include "system/eventlog.h"
#include "system/odometer.h"
#include "system/timeutil.h"

namespace web_history {
namespace {

WebServer* g_http = nullptr;

// Series writers stream straight to the socket. They used to append to an
// Arduino String whose reserve()/concat() failures nothing checked; with
// per-row timestamps and all seven series the full 288-row body runs past
// 11 KB, and this board's largest contiguous free block has measured 7.7 KB.
// See http_tx::Chunked.
void write_series(http_tx::Chunked& tx, const char* name, const float* v, uint16_t n,
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

// Gas columns: -1 (no sensor / pre-sensor rows) serializes as null so the
// chart shows absence, not a plausible -1. 0 (algorithm warming) is a VALUE.
template <typename T>
void write_gas(http_tx::Chunked& tx, const char* name, const T* v, uint16_t n) {
  tx.printf("\"%s\":[", name);
  for (uint16_t i = 0; i < n && tx.ok(); i++) {
    if (v[i] < 0)
      tx.print(i + 1 < n ? "null," : "null");
    else
      tx.printf(i + 1 < n ? "%ld," : "%ld", static_cast<long>(v[i]));
  }
  tx.print("]");
}

void write_ints(http_tx::Chunked& tx, const char* name, const int8_t* v, uint16_t n) {
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
void write_ts(http_tx::Chunked& tx, const time_t* v, uint16_t n) {
  tx.print("\"ts\":[");
  for (uint16_t i = 0; i < n && tx.ok(); i++) {
    tx.printf(i + 1 < n ? "%ld," : "%ld", static_cast<long>(v[i]));
  }
  tx.print("]");
}

// The ring's rows are uniformly spaced by construction, so its timestamps are
// derived rather than stored -- no scratch array for them.
void write_ts_derived(http_tx::Chunked& tx, time_t end, uint16_t n, long step) {
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
struct GraphScratch {
  time_t* ts = nullptr;
  float* t = nullptr;
  float* h = nullptr;
  float* p = nullptr;
  float* o = nullptr;
  float* b = nullptr;
  float* w = nullptr;
  int32_t* vr = nullptr;
  int32_t* nr = nullptr;
  int16_t* vi = nullptr;
  int16_t* ni = nullptr;
  int8_t* sp = nullptr;
  int8_t* cg = nullptr;
  int8_t* f = nullptr;
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
  g_scratch.w = psram_array<float>(kGraphMaxPts);
  g_scratch.vr = psram_array<int32_t>(kGraphMaxPts);
  g_scratch.nr = psram_array<int32_t>(kGraphMaxPts);
  g_scratch.vi = psram_array<int16_t>(kGraphMaxPts);
  g_scratch.ni = psram_array<int16_t>(kGraphMaxPts);
  g_scratch.f = psram_array<int8_t>(kGraphMaxPts);
  g_scratch.ready = g_scratch.ts && g_scratch.t && g_scratch.h && g_scratch.p && g_scratch.o &&
                    g_scratch.b && g_scratch.sp && g_scratch.cg && g_scratch.w && g_scratch.vr &&
                    g_scratch.nr && g_scratch.vi && g_scratch.ni && g_scratch.f;
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
    free(g_scratch.w);
    free(g_scratch.vr);
    free(g_scratch.nr);
    free(g_scratch.vi);
    free(g_scratch.ni);
    free(g_scratch.f);
    g_scratch = GraphScratch{};
    eventlog::log("web", "graph scratch alloc failed");
  }
  return g_scratch.ready;
}

// Every series both /api/history branches emit, in wire order. One emitter,
// two sources: the SD path fills a SeriesView from the PSRAM scratch, the
// ring path from the ring's accessors. The list living in exactly one place
// is what keeps the two branches from drifting apart (the contract test
// checks the names; this keeps the ORDER and the formatting honest too).
struct SeriesView {
  const float* t;
  const float* h;
  const float* p;
  const float* o;
  const float* b;
  const float* w;
  const int8_t* sp;
  const int8_t* cg;
  const int32_t* vr;
  const int32_t* nr;
  const int16_t* vi;
  const int16_t* ni;
  const int8_t* f;
};

void write_all_series(http_tx::Chunked& tx, const SeriesView& v, uint16_t n) {
  tx.print(",");
  write_series(tx, WN_TEMP_C, v.t, n, 1);
  tx.print(",");
  write_series(tx, WN_RH, v.h, n, 0);
  tx.print(",");
  write_series(tx, WN_HPA, v.p, n, 1);
  tx.print(",");
  write_series(tx, WN_OUT_F, v.o, n, 1);
  tx.print(",");
  write_series(tx, WN_BATT_V, v.b, n, 2);
  tx.print(",");
  write_ints(tx, WN_SPD, v.sp, n);
  tx.print(",");
  write_ints(tx, WN_CHG, v.cg, n);
  tx.print(",");
  write_series(tx, WN_WATTS, v.w, n, 1);
  tx.print(",");
  write_gas(tx, WN_VOC_RAW, v.vr, n);
  tx.print(",");
  write_gas(tx, WN_NOX_RAW, v.nr, n);
  tx.print(",");
  write_gas(tx, WN_VOC, v.vi, n);
  tx.print(",");
  write_gas(tx, WN_NOX, v.ni, n);
  tx.print(",");
  // Same encoding as the gas columns: -1 (no meter / pre-1.21.0 row) is null.
  write_gas(tx, WN_FLIPS, v.f, n);
  tx.print("}");
}

// The CSV header, once: /download.csv writes it on both the card path and
// the ring fallback, and two literals had already started to count as
// duplication before they could start to disagree.
// clang-format off
constexpr const char* kCsvHeader = "epoch,temp_c,rh,hpa,outside_f,speed,batt_v,chg,watts,voc_raw,nox_raw,voc,nox,flips\n";  // NOLINT(whitespace/line_length)
// clang-format on

void handle_history() {
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
  const String days_arg = g_http->hasArg("days") ? g_http->arg("days") : "";
  const int days = days_arg.toInt();
  if (days_arg != "1" && days_arg != "7" && days_arg != "30" && days_arg != "60") {
    g_http->send(400, "application/json", "{\"error\":\"days must be 1, 7, 30 or 60\"}");
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
    g_http->send(503, "application/json",
                 sdcard::ok() ? "{\"error\":\"clock not synced yet\"}"
                              : "{\"error\":\"sd card not mounted\"}");
    return;
  }

  if (have_card && !scratch_ready()) {
    g_http->send(503, "application/json", "{\"error\":\"out of memory for chart data\"}");
    return;
  }

  const long step = (long)(kSampleMs / 1000);
  http_tx::Chunked tx(g_http->client(), "application/json");
  if (have_card) {
    const GraphScratch& s = g_scratch;
    const sdcard::Samples dst{s.ts, s.t, s.h,  s.p,  s.o,  s.b,  s.sp,
                              s.cg, s.w, s.vr, s.nr, s.vi, s.ni, s.f};
    const time_t cutoff = time(nullptr) - (time_t)days * 86400;
    const uint16_t n = sdcard::read_range(cutoff, dst, kGraphMaxPts);
    // interval_s is now only a nominal hint for gap detection; ts[] carries
    // the truth. It used to be days*86400/n, which inflated by exactly
    // requested_span/actual_span whenever the card held less than the window.
    tx.printf("{\"source\":\"sd\",\"interval_s\":%ld,", step);
    write_ts(tx, s.ts, n);
    write_all_series(tx, {s.t, s.h, s.p, s.o, s.b, s.w, s.sp, s.cg, s.vr, s.nr, s.vi, s.ni, s.f},
                     n);
  } else {
    // No card and days=1: the ring is all there is, and the response says so
    // rather than letting the caller assume persistence it does not have.
    const uint16_t rows = history::count();
    tx.printf("{\"source\":\"ring\",\"interval_s\":%ld,", step);
    write_ts_derived(tx, history::end_ts(), rows, step);
    write_all_series(
        tx,
        {history::temp(), history::rh(), history::hpa(), history::out_f(), history::batt_v(),
         history::watts(), history::speed(), history::chg(), history::voc_raw(), history::nox_raw(),
         history::voc(), history::nox(), history::flips()},
        rows);
  }
  tx.end();
}

void handle_csv() {
  // Same rule as handle_history: a malformed question gets an error, not a
  // plausible answer. This used to clamp silently, so ?days=90 and ?days=abc
  // both returned a 30-day export under a 200 with a filename claiming 30 days.
  int days = 30;
  if (g_http->hasArg("days")) {
    const String raw = g_http->arg("days");
    days = raw.toInt();
    if (days < 1 || days > 60 || String(days) != raw) {
      g_http->send(400, "application/json", "{\"error\":\"days must be 1-60\"}");
      return;
    }
  }

  if (sdcard::ok() && time_synced()) {
    char disp[96];
    snprintf(disp, sizeof(disp), "Content-Disposition: attachment; filename=garage-fan-%dd.csv\r\n",
             days);
    http_tx::Chunked tx(g_http->client(), "text/csv", disp);
    // Header names the full 1.14.48 column set; older rows in the same file
    // carry 6, 8 or 13 fields and are streamed exactly as stored rather than
    // back-filled -- a short row simply has no values under the later labels.
    tx.print(kCsvHeader);
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
  http_tx::Chunked tx(g_http->client(), "text/csv",
                      "Content-Disposition: attachment; filename=garage-fan-ring.csv\r\n");
  tx.print(kCsvHeader);
  for (uint16_t i = 0; i < n && tx.ok(); i++) {
    const long ts =
        history::end_ts() ? (long)history::end_ts() - (long)(n - 1 - i) * (kSampleMs / 1000) : 0;
    tx.printf("%ld,%.2f,%.0f,%.1f,%.1f,%d,%.2f,%d,%.1f,%ld,%ld,%d,%d,%d\n", ts, history::temp()[i],
              history::rh()[i], history::hpa()[i],
              isnan(history::out_f()[i]) ? -999.0f : history::out_f()[i],
              static_cast<int>(history::speed()[i]),
              isnan(history::batt_v()[i]) ? -999.0f : history::batt_v()[i],
              static_cast<int>(history::chg()[i]),
              isnan(history::watts()[i]) ? -999.0f : history::watts()[i],
              (long)history::voc_raw()[i], (long)history::nox_raw()[i],
              static_cast<int>(history::voc()[i]), static_cast<int>(history::nox()[i]),
              static_cast<int>(history::flips()[i]));
  }
  tx.end();
}

// GET /api/boots?days=N -- the restart marks the charts hang their outage
// labels on. Same day validation as /api/history: a malformed question gets
// an error, never a plausible-looking answer for a window nobody asked for.
void handle_boots() {
  const String days_arg = g_http->hasArg("days") ? g_http->arg("days") : "";
  if (days_arg != "1" && days_arg != "7" && days_arg != "30" && days_arg != "60") {
    g_http->send(400, "application/json", "{\"error\":\"days must be 1, 7, 30 or 60\"}");
    return;
  }
  if (!sdcard::ok() || !time_synced()) {
    g_http->send(503, "application/json",
                 sdcard::ok() ? "{\"error\":\"clock not synced yet\"}"
                              : "{\"error\":\"sd card not mounted\"}");
    return;
  }
  const time_t cutoff = time(nullptr) - static_cast<time_t>(days_arg.toInt()) * 86400;
  http_tx::Chunked tx(g_http->client(), "application/json");
  tx.print("{" WK_BOOTS "[");
  // The sink parses each stored line and re-emits it as an object: the file
  // is the device's own format, the wire is types.ts's.
  struct Ctx {
    http_tx::Chunked* tx;
    uint32_t n;
  } ctx{&tx, 0};
  bootlog::stream(
      cutoff,
      [](const char* line, void* raw) {
        auto* c = static_cast<Ctx*>(raw);
        long ts = 0;
        unsigned long boots = 0;
        char cause[24] = "";
        // %23[^\n,] stops at the separator AND the newline, so a truncated
        // final line cannot drag the next record into this one's cause.
        if (sscanf(line, "%ld,%lu,%23[^\n,]", &ts, &boots, cause) < 2)
          return true;  // skip a malformed row rather than tearing the body
        // The cause lands inside a JSON string, and the file is bytes on a
        // card that anything could have written -- a quote or a backslash in
        // there would invalidate the WHOLE response. crashlog's vocabulary is
        // words and underscores, so anything else is corruption: drop it to
        // '_' rather than escaping, which keeps the value readable and the
        // document well-formed by construction.
        for (char* p = cause; *p; ++p) {
          const bool safe = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                            (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == ' ';
          if (!safe)
            *p = '_';
        }
        if (!c->tx->printf(c->n ? ",{" WK_TS "%ld," WK_N "%lu," WK_CAUSE "\"%s\"}"
                                : "{" WK_TS "%ld," WK_N "%lu," WK_CAUSE "\"%s\"}",
                           ts, boots, cause[0] ? cause : "unknown"))
          return false;
        c->n++;
        return true;
      },
      &ctx);
  tx.print("]}");
  tx.end();
}

void handle_stats() {
  float tmin, tmax, tavg;
  history::temp_stats(&tmin, &tmax, &tavg);
  char buf[288];
  snprintf(buf, sizeof(buf),
           "{" WK_RUN_TODAY_S "%lu," WK_RUN_TOTAL_S "%lu," WK_ENERGY_WH "%.0f," WK_WH_TODAY
           "%.1f," WK_WATTS_NOW "%.0f," WK_T_MIN_F "%.1f," WK_T_MAX_F "%.1f," WK_T_AVG_F
           "%.1f," WK_SAMPLES "%u}",
           (unsigned long)odometer::run_today_s(), (unsigned long)odometer::run_total_s(),
           odometer::energy_wh(), odometer::wh_today(),
           fan::watts(fan::speed() < 0 ? 0 : fan::speed()), isnan(tmin) ? 0 : tmin * 9 / 5 + 32,
           isnan(tmax) ? 0 : tmax * 9 / 5 + 32, isnan(tavg) ? 0 : tavg * 9 / 5 + 32,
           history::count());
  g_http->send(200, "application/json", buf);
}

}  // namespace

void register_routes(WebServer& http) {
  g_http = &http;
  http.on("/api/history", handle_history);
  http.on("/api/stats", handle_stats);
  http.on("/api/boots", handle_boots);
  http.on("/download.csv", handle_csv);
}

}  // namespace web_history
