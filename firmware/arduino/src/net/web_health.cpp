#include "net/web_health.h"

#include <cmath>
#include <cstdio>

#include "fan/control.h"
#include "net/health_status.h"
#include "net/mqtt_link.h"
#include "net/plug.h"
#include "sensors/climate.h"
#include "system/ota_rollback.h"

namespace web_health {
namespace {

WebServer* g_http = nullptr;

void handle_health() {
  const long meter_age = plug::age_s();
  const bool power_ok = !plug::enabled() || (meter_age >= 0 && meter_age <= 120 &&
                                             plug::verdict() != -1 && !plug::cycling());
  const health_status::Snapshot s = {mqtt_link::connected(),
                                     ota_rollback_image_confirmed(),
                                     fan::auto_on(),
                                     std::isfinite(climate::inside_c()),
                                     std::isfinite(climate::outside_c_fresh()),
                                     power_ok,
                                     !fan::output_fault()};
  const bool ready = health_status::ready(s);
  char body[256];
  snprintf(body, sizeof(body),
           "{\"status\":\"%s\",\"mqtt\":%s,\"confirmed\":%s,\"auto\":%s,"
           "\"inside_fresh\":%s,\"outside_fresh\":%s,\"power_ok\":%s,\"actuator_ok\":%s}",
           ready ? "up" : "down", s.mqtt ? "true" : "false", s.confirmed ? "true" : "false",
           s.automatic ? "true" : "false", s.inside_fresh ? "true" : "false",
           s.outside_fresh ? "true" : "false", s.power_ok ? "true" : "false",
           s.actuator_ok ? "true" : "false");
  g_http->sendHeader("Cache-Control", "no-store");
  g_http->send(ready ? 200 : 503, "application/json", body);
}

}  // namespace

void register_routes(WebServer& http) {
  g_http = &http;
  http.on("/health", HTTP_GET, handle_health);
}

}  // namespace web_health
