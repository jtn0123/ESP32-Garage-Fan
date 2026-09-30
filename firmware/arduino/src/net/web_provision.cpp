// See web_provision.h.
#include "net/web_provision.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>

#include "net/creds.h"
#include "net/web_gate.h"
#include "system/eventlog.h"

namespace web_provision {
namespace {

WebServer* g_http = nullptr;
const char* g_token = "";

void handle_provision() {
  // The wire arguments, in the order the form presents them. Mirrored by
  // PROVISION_KEYS in scripts/mock_device.py; tests/test_web_contract.py pins
  // the two lists to each other.
  static const char* const kArgs[] = {"ssid",      "pass",      "mqtt_host", "mqtt_port",
                                      "mqtt_user", "mqtt_pass", "lat",       "lon"};
  if (!web_gate::guard_origin(*g_http))
    return;
  if (!web_gate::guard_token(*g_http, g_token))
    return;
  // Keep String storage alive throughout validation and the single commit.
  String values[8];
  creds::Input fields[8];
  size_t given = 0;
  char applied[96] = "";
  size_t n = 0;
  for (const char* arg : kArgs) {
    if (!g_http->hasArg(arg))
      continue;
    values[given] = g_http->arg(arg);
    fields[given] = {arg, values[given].c_str()};
    ++given;
    n += snprintf(applied + n, sizeof(applied) - n, n ? ",%s" : "%s", arg);
  }
  if (given == 0) {
    g_http->send(400, "application/json", "{\"error\":\"no fields\"}");
    return;
  }
  const char* bad = "field";
  const creds::Result result = creds::apply_fields(fields, given, &bad);
  if (result != creds::Result::Ok) {
    char body[80];
    if (result == creds::Result::StorageFailure)
      snprintf(body, sizeof(body), "{\"error\":\"credentials could not be saved\"}");
    else
      snprintf(body, sizeof(body), "{\"error\":\"bad %s\"}", bad);
    g_http->send(result == creds::Result::StorageFailure ? 503 : 400, "application/json", body);
    return;
  }
  // Field NAMES to the flight recorder, never values: this line is what
  // explains "the fan rebooted at 14:02 and came back on a new SSID".
  eventlog::log("creds", "provisioned %s; rebooting", applied);
  eventlog::flush_tick();
  g_http->send(200, "application/json",
               "{\"ok\":true,\"note\":\"rebooting with the new credentials\"}");
  delay(150);  // let the response drain before the reset takes the socket
  esp_restart();
}

}  // namespace

void register_routes(WebServer& http, const char* token) {
  g_http = &http;
  g_token = token;
  http.on("/api/provision", HTTP_POST, handle_provision);
}

}  // namespace web_provision
