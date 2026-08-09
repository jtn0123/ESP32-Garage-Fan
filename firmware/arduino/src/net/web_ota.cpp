// See web_ota.h. Handler bodies verbatim from web.cpp; the flight-recorder
// lines are the only addition.
#include "net/web_ota.h"

#include <Arduino.h>
#include <Update.h>

#include "system/eventlog.h"

namespace web_ota {
namespace {

WebServer* g_http = nullptr;
const char* g_token = nullptr;
bool g_authorized = false;

// Same rule as web.cpp's token_ok: an empty configured token authorizes
// nothing.
bool token_ok(const String& presented) {
  return g_token && g_token[0] != '\0' && presented == g_token;
}

void handle_upload() {
  HTTPUpload& up = g_http->upload();
  if (up.status == UPLOAD_FILE_START) {
    g_authorized = token_ok(g_http->arg("token"));
    if (!g_authorized) {
      eventlog::log("ota", "rejected: bad token");
      return;
    }
    eventlog::log("ota", "receiving %s", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
      Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE && g_authorized) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize)
      Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END && g_authorized) {
    if (Update.end(true))
      eventlog::log("ota", "received %u bytes ok", up.totalSize);
    else
      Update.printError(Serial);
  }
}

void handle_done() {
  if (!g_authorized) {
    g_http->send(403, "application/json", "{\"error\":\"bad token\"}");
    return;
  }
  if (Update.hasError()) {
    eventlog::log("ota", "update failed");
    g_http->send(500, "application/json", "{\"error\":\"update failed\"}");
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
