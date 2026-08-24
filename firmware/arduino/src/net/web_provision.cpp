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
  // Validate everything before storing anything: a half-applied set of
  // credentials is the one outcome worse than a rejected one.
  char bad[24] = "";
  int given = 0;
  for (const char* arg : kArgs) {
    if (!g_http->hasArg(arg))
      continue;
    given++;
    const String v = g_http->arg(arg);
    // Dry-run the validation rules without persisting: set_field persists,
    // so check the cheap invariants here and let set_field be authoritative
    // for length once we commit.
    if (strcmp(arg, "ssid") == 0 && v.length() == 0) {
      snprintf(bad, sizeof(bad), "%s", arg);
      break;
    }
    if (strcmp(arg, "mqtt_port") == 0) {
      const long p = v.toInt();
      if (p < 1 || p > 65535) {
        snprintf(bad, sizeof(bad), "%s", arg);
        break;
      }
    }
  }
  if (bad[0] != '\0') {
    char body[64];
    snprintf(body, sizeof(body), "{\"error\":\"bad %s\"}", bad);
    g_http->send(400, "application/json", body);
    return;
  }
  if (given == 0) {
    g_http->send(400, "application/json", "{\"error\":\"no fields\"}");
    return;
  }
  char applied[96] = "";
  size_t n = 0;
  for (const char* arg : kArgs) {
    if (!g_http->hasArg(arg))
      continue;
    if (!creds::set_field(arg, g_http->arg(arg).c_str())) {
      // Over-long value: storing stopped here, the earlier fields stand.
      // Name it so the form can say which one rather than "failed".
      char body[64];
      snprintf(body, sizeof(body), "{\"error\":\"bad %s\"}", arg);
      g_http->send(400, "application/json", body);
      return;
    }
    if (n < sizeof(applied))
      n += snprintf(applied + n, sizeof(applied) - n, n ? ",%s" : "%s", arg);
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
