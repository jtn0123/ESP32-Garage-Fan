// See web_ota.h.
//
// The failure this file is written around: WebServer's upload callback can end
// in UPLOAD_FILE_ABORTED (Parsing.cpp `_parseFormUploadAborted`), and that path
// returns false from _parseRequest -- so WebServer.cpp never calls
// _handleRequest, and handle_done never runs. Anything this module leaves
// behind at that moment therefore survives for the rest of the boot.
//
// Two things used to survive it, and together they made an interrupted upload
// (Ctrl-C on the deploy script, a WiFi drop mid-flash) permanently destructive:
//
//   1. g_authorized stayed true, so a later body-less POST /update from anyone
//      on the LAN reached handle_done's reboot with no token at all.
//   2. Update was left running. UpdateClass::begin() returns false on
//      `_size > 0` WITHOUT setting _error, so every subsequent OTA that boot
//      no-opped while Update.hasError() stayed false -- and this endpoint
//      answered {"ok":true,"note":"rebooting into new slot"} and rebooted into
//      the OLD image. The device was silently un-updatable until a power cycle,
//      and the response said the opposite.
//
// Hence: an ABORTED branch that resets both, and a g_failed flag so a begin()
// or write() failure is reported as a failure instead of being printed to a
// serial port that CLAUDE.md notes is usually not attached.
#include "net/web_ota.h"

#include <Arduino.h>
#include <Update.h>

#include "esp_task_wdt.h"
#include "system/eventlog.h"

namespace web_ota {
namespace {

WebServer* g_http = nullptr;
const char* g_token = nullptr;
bool g_authorized = false;
// Set when any stage of the stream failed. Update.hasError() is not sufficient
// on its own: the "already running" rejection from begin() leaves _error clear.
bool g_failed = false;
const char* g_fail_reason = nullptr;

// Same rule as web.cpp's token_ok: an empty configured token authorizes
// nothing.
bool token_ok(const String& presented) {
  return g_token && g_token[0] != '\0' && presented == g_token;
}

// Put the Update engine and this module's flags back to a state where the NEXT
// upload can succeed. Update.abort() clears _size, which is what makes
// begin() willing to start again.
void reset_stream(const char* reason) {
  if (Update.isRunning())
    Update.abort();
  g_authorized = false;
  g_failed = reason != nullptr;
  g_fail_reason = reason;
}

void handle_upload() {
  HTTPUpload& up = g_http->upload();
  if (up.status == UPLOAD_FILE_START) {
    g_failed = false;
    g_fail_reason = nullptr;
    g_authorized = token_ok(g_http->arg("token"));
    if (!g_authorized) {
      eventlog::log("ota", "rejected: bad token");
      return;
    }
    eventlog::log("ota", "receiving %s", up.filename.c_str());
    // A previous aborted upload can leave the engine running; clear it first
    // so this attempt is not rejected by the `_size > 0` guard.
    if (Update.isRunning())
      Update.abort();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      eventlog::log("ota", "begin failed: %s", Update.errorString());
      g_failed = true;
      g_fail_reason = "flash begin failed";
    }
  } else if (up.status == UPLOAD_FILE_WRITE && g_authorized && !g_failed) {
    // handleClient() consumes the whole upload in one call, so loop() cannot
    // feed the task watchdog until the flash finishes. A slow WiFi upload
    // (>60 s) then panics the board mid-OTA (seen 2026-08-09). Chunks still
    // arriving means progress, so feed it here.
    esp_task_wdt_reset();
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      Update.printError(Serial);
      eventlog::log("ota", "write failed: %s", Update.errorString());
      g_failed = true;
      g_fail_reason = "flash write failed";
    }
  } else if (up.status == UPLOAD_FILE_END && g_authorized && !g_failed) {
    if (Update.end(true)) {
      eventlog::log("ota", "received %u bytes ok", up.totalSize);
    } else {
      Update.printError(Serial);
      eventlog::log("ota", "end failed: %s", Update.errorString());
      g_failed = true;
      g_fail_reason = "image rejected";
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    // handle_done will NOT run for this request. Everything that must not
    // outlive the attempt has to be cleared right here.
    eventlog::log("ota", "upload aborted after %u bytes", up.totalSize);
    reset_stream("upload aborted");
  }
}

void handle_done() {
  // Consume the flag: module state outlives requests, and a body-less POST
  // never runs the upload handler at all -- without this, one authorized
  // upload that failed mid-flash would let the NEXT caller reboot the board
  // tokenless.
  const bool authorized = g_authorized;
  const bool failed = g_failed;
  const char* reason = g_fail_reason ? g_fail_reason : "update failed";
  g_authorized = false;
  g_failed = false;
  g_fail_reason = nullptr;
  if (!authorized) {
    g_http->send(403, "application/json", "{\"error\":\"bad token\"}");
    return;
  }
  // isRunning() is _size > 0, and a successful end() runs _reset() which zeroes
  // it -- so a stream still "running" here is one whose UPLOAD_FILE_END never
  // arrived. Reporting that as success is what let a half-flashed image claim
  // it had rebooted into a new slot.
  if (failed || Update.hasError() || Update.isRunning()) {
    // Leave nothing running for the next attempt to trip over.
    if (Update.isRunning())
      Update.abort();
    eventlog::log("ota", "update failed: %s", reason);
    char body[96];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", reason);
    g_http->send(500, "application/json", body);
    return;
  }
  eventlog::log("ota", "flashed; rebooting into new slot");
  g_http->send(200, "application/json", "{\"ok\":true,\"note\":\"rebooting into new slot\"}");
  delay(300);
  esp_restart();
}

}  // namespace

void register_routes(WebServer& http, const char* token) {
  g_http = &http;
  g_token = token;
  http.on("/update", HTTP_POST, handle_done, handle_upload);
}

}  // namespace web_ota
