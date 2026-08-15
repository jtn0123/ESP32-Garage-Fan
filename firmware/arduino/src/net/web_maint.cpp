// See web_maint.h. Handler bodies verbatim from web.cpp at the split; only
// the WebServer reference and the shared gates (web_gate.h) are indirected.
#include "net/web_maint.h"

#include <Arduino.h>
#include <SD.h>

#include <cstdio>

#include "esp_task_wdt.h"
#include "net/base64_stream.h"
#include "net/http_tx.h"
#include "net/web_gate.h"
#include "storage/sdcard.h"
#include "system/coredump.h"
#include "system/eventlog.h"
#include "ui/display.h"

namespace web_maint {
namespace {

WebServer* g_http = nullptr;
const char* g_token = "";

bool guard_origin() { return web_gate::guard_origin(*g_http); }
bool guard_token() { return web_gate::guard_token(*g_http, g_token); }

// The last panic, decoded on-device.
//
// Answers the question the flight recorder could not: a panic used to reach us
// as the bare word "panic" because IDF prints the backtrace over a UART this
// board's CDC drops. The dump was always in flash; see system/coredump.h.
// Both core-dump reads are token-gated, unlike the rest of the read-only API.
// A core dump is a snapshot of RAM at the moment of the fault, and RAM at that
// moment holds the token itself, plus the WiFi and MQTT credentials. Serving it
// to any LAN client hands over the exact secret that guards OTA -- a read that
// escalates to full control of the board. Origin checks do not help here: they
// stop a browser on another site from FORGING a request, not a client that
// simply asks. /api/crash/erase was already gated; these two were not.
void handle_crash() {
  if (!guard_token())
    return;
  char buf[896];
  const size_t n = coredump::to_json(buf, sizeof(buf));
  if (n == 0) {
    g_http->send(500, "application/json", "{\"error\":\"could not read the core dump\"}");
    return;
  }
  g_http->send(200, "application/json", buf);
}

// The raw ELF core dump, for scripts/decode_crash.py to turn the backtrace PCs
// into file:line against the matching build.
void handle_crash_raw() {
  if (!guard_token())
    return;
  size_t addr = 0, size = 0;
  if (!coredump::image_range(&addr, &size) || size == 0) {
    g_http->send(404, "application/json", "{\"error\":\"no core dump stored\"}");
    return;
  }
  http_tx::Chunked tx(g_http->client(), "application/octet-stream",
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
void handle_crash_erase() {
  if (!guard_origin())
    return;
  if (!guard_token())
    return;
  const bool ok = coredump::erase();
  eventlog::log("crash", "core dump erased by request (%s)", ok ? "ok" : "failed");
  g_http->send(ok ? 200 : 500, "application/json",
               ok ? "{\"ok\":true}" : "{\"error\":\"erase failed\"}");
}

/**
 * GET /api/display -- the panel's last frame, so the console can mirror it.
 *
 * Two 1-bit planes, base64'd, in OUR row-major format rather than the driver's
 * column-major rotated one (see ui/display.h). These are the same bytes that
 * were clocked to the glass, so the browser cannot drift from the device: a
 * re-implementation of the layout in JS would look right and be wrong the first
 * time either side changed, which is exactly what the deleted UI-codegen
 * pipeline existed to manage.
 *
 * Streamed: the two planes are 3904 bytes each and base64 inflates them to
 * ~10.4 KB, well past the largest contiguous block this board can promise.
 */
void handle_display() {
  const uint8_t* black = display::plane_black();
  const uint8_t* red = display::plane_red();
  if (!black || !red) {
    // Not an error: the panel simply has not painted yet. The console shows
    // "waiting for the first refresh" rather than an empty frame that looks
    // like a broken display.
    g_http->send(200, "application/json",
                 "{\"ready\":false,\"w\":0,\"h\":0,\"stride\":0,\"tricolor\":false,"
                 "\"age_s\":-1,\"black\":\"\",\"red\":\"\"}");
    return;
  }
  http_tx::Chunked tx(g_http->client(), "application/json");
  tx.printf("{\"ready\":true,\"w\":%u,\"h\":%u,\"stride\":%u,\"tricolor\":%s,\"age_s\":%ld,",
            (unsigned)display::width(), (unsigned)display::height(), (unsigned)display::stride(),
            display::tricolor() ? "true" : "false", (long)display::age_s());
  const size_t n = display::plane_bytes();
  auto sink = [&tx](const char* p, size_t len) { return tx.write(p, len); };
  tx.print("\"black\":\"");
  base64_stream::encode(black, n, sink);
  tx.print("\",\"red\":\"");
  base64_stream::encode(red, n, sink);
  tx.print("\"}");
  // A read failure mid-body must NOT be terminated as a complete document --
  // the console would parse a truncated frame as a real one. Same rule as
  // /api/crash.bin.
  if (!tx.ok())
    tx.abort();
}

/**
 * POST /api/display/refresh -- repaint now instead of waiting for the cadence.
 *
 * 429 when refused. A refresh parks the loop for seconds (16.7 s measured on
 * this panel), so display::request_refresh() keeps a floor between forced
 * repaints; answering 200 to a refusal would have the console report a repaint
 * that never happened.
 */
void handle_display_refresh() {
  if (!guard_origin())
    return;
  if (!display::request_refresh()) {
    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":false,\"retry_in_s\":%lu}",
             (unsigned long)display::forced_retry_in_s());
    g_http->send(429, "application/json", body);
    return;
  }
  g_http->send(200, "application/json", "{\"ok\":true,\"retry_in_s\":0}");
}

// Token-guarded reboot behind the console's maintenance row. Shares the OTA
// token so reaching the page is not by itself enough to bounce the fan.
void handle_restart() {
  if (!guard_origin())
    return;
  if (!guard_token())
    return;
  eventlog::log("web", "restart requested");
  eventlog::flush_tick();  // best effort: land the reason on SD first
  g_http->send(200, "application/json", "{\"ok\":true,\"note\":\"restarting\"}");
  delay(150);  // let the response drain before the reset takes the socket
  esp_restart();
}

// Token-guarded one-shot: mount with format-on-failure, turning a fresh
// exFAT card into FAT32 in place. Deliberately NOT automatic on normal
// mounts -- a flaky-but-full card must never be silently erased. Blocks the
// loop for the duration (can be minutes on big cards); the LEDC peripheral
// keeps the fan running throughout.
void handle_sd_format() {
  if (!guard_origin())
    return;
  if (!guard_token())
    return;
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
  g_http->send(mounted ? 200 : 500, "application/json", buf);
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
void handle_sd_purge() {
  if (!guard_origin())
    return;
  if (!guard_token())
    return;
  if (!sdcard::ok()) {
    g_http->send(503, "application/json", "{\"error\":\"sd card not mounted\"}");
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
  g_http->send(200, "application/json", buf);

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
  // The fan itself is unaffected -- the LEDC peripheral holds its duty across a
  // reset, and the console's own restart button already works this way.
  eventlog::log("sd", "purge done; restarting to rebuild filesystem state");
  eventlog::flush_tick();
  delay(200);
  esp_restart();
}

// The flight recorder. The RAM ring by default (works with no card, this
// boot only); ?sd=1 tails /events.log for the record that survives reboots.
void handle_events() {
  if (g_http->hasArg("sd")) {
    // 2 KB, static (does not belong on the loop stack): ~20 recent lines of
    // tail is plenty for remote triage, and this board's heap is tight
    // enough that every resident KB fights the SD mount for its slab.
    static char buf[2048];
    const uint32_t n = sdcard::tail_events(buf, sizeof(buf));
    http_tx::send_big(g_http->client(), "text/plain", buf, n);
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
  http_tx::send_big(g_http->client(), "text/plain", out.c_str(), out.length());
}

// The whole flight-recorder file, not the 2 KB tail. Exists because the
// sample log showed overnight 15-minute gaps (reboot + slow SNTP skips CSV
// rows) and the only record of WHY the board rebooted at 22:37 was sitting in
// /events.log with no way to read more than its last twenty lines.
void handle_events_file() {
  if (!sdcard::ok()) {
    g_http->send(503, "application/json", "{\"error\":\"no card\"}");
    return;
  }
  File f = SD.open("/events.log", FILE_READ);
  if (!f) {
    g_http->send(404, "application/json", "{\"error\":\"no events.log\"}");
    return;
  }
  http_tx::Chunked tx(g_http->client(), "text/plain", "");
  char buf[512];
  while (true) {
    const int got = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
    if (got <= 0)
      break;
    if (!tx.write(buf, static_cast<size_t>(got)))
      break;
    esp_task_wdt_reset();  // ~512 KB worst case through a slow peer
  }
  f.close();
  tx.end();
}

}  // namespace

void register_routes(WebServer& http, const char* token) {
  g_http = &http;
  g_token = token;
  // POST-only where the route CHANGES the device: a route registered without
  // a method answers GET too, which made every one of them reachable from any
  // web page the operator had open (see web.cpp's registration block).
  http.on("/api/restart", HTTP_POST, handle_restart);
  http.on("/api/sdformat", HTTP_POST, handle_sd_format);
  http.on("/api/sdpurge", HTTP_POST, handle_sd_purge);
  http.on("/api/events", handle_events);
  http.on("/events.log", handle_events_file);
  http.on("/api/display", handle_display);
  http.on("/api/display/refresh", HTTP_POST, handle_display_refresh);
  http.on("/api/crash", handle_crash);
  http.on("/api/crash.bin", handle_crash_raw);
  http.on("/api/crash/erase", HTTP_POST, handle_crash_erase);
}

}  // namespace web_maint
