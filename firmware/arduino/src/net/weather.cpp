// See weather.h. One plain-HTTP GET against api.open-meteo.com, parsed with
// a bare string scan -- the response is ~300 bytes of JSON and the only field
// this module wants is current.temperature_2m.
//
// HTTP, not HTTPS, deliberately: mbedtls needs ~34 KB of CONTIGUOUS internal
// RAM for its record buffers (PSRAM cannot host them in the prebuilt Arduino
// libs), and the very first TLS attempt on this board died with "SSL - Memory
// allocation failed" (2026-08-10, v1.14.19 first flash). The payload is a
// public weather forecast for coordinates already rounded to ~1 km by the
// header generator; the worst an on-path attacker can do is nudge a garage
// fan's speed, and the plausibility clamp below bounds even that.
#include "net/weather.h"

#include <Arduino.h>
#include <WiFi.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "config.h"
#include "generated_config.h"
#include "sensors/climate.h"
#include "system/eventlog.h"
#include "system/timeutil.h"

namespace weather {
namespace {

constexpr uint32_t kPollMs = 10 * 60 * 1000;   // half the 30-min staleness window:
                                               // one failed fetch never strands auto
constexpr uint32_t kFirstPollDelayMs = 25000;  // let WiFi/MQTT/SD settle first

uint32_t g_last_ms = 0;
bool g_ever = false;

// GET the current temperature; NAN on any failure, with a short reason in
// `err` -- the MQTT feed died silently for five days and nobody could tell,
// so this path's failures are required to be legible.
float fetch_f(uint32_t* dur_ms, char* err, size_t err_cap) {
  const uint32_t t0 = millis();
  WiFiClient net;
  net.setTimeout(10000);
  float out = NAN;
  snprintf(err, err_cap, "connect failed");
  if (net.connect("api.open-meteo.com", 80)) {
    net.print("GET /v1/forecast?latitude=" WEATHER_LAT "&longitude=" WEATHER_LON
              "&current=temperature_2m&temperature_unit=fahrenheit HTTP/1.1\r\n"
              "Host: api.open-meteo.com\r\n"
              "User-Agent: garage-fan/" FW_VERSION
              "\r\n"
              "Connection: close\r\n\r\n");
    // Small bounded read: headers + body fit comfortably; anything beyond the
    // buffer is truncated, and the field either parses or it does not.
    char buf[1536];
    size_t n = 0;
    const uint32_t t_read = millis();
    while (n < sizeof(buf) - 1 && millis() - t_read < 10000) {
      if (!net.available()) {
        if (!net.connected())
          break;
        delay(10);
        continue;
      }
      const int got = net.read(reinterpret_cast<uint8_t*>(buf) + n, sizeof(buf) - 1 - n);
      if (got > 0)
        n += static_cast<size_t>(got);
    }
    buf[n] = '\0';
    if (strncmp(buf, "HTTP/1.1 200", 12) == 0) {
      snprintf(err, err_cap, "no temperature in body");
      // The units object also contains the key ("temperature_2m":"\u00b0F");
      // skip matches whose value is not a number.
      const char* k = strstr(buf, "\"temperature_2m\":");
      while (k) {
        const char* v = k + 17;  // strlen("\\"temperature_2m\\":") -- k+18 would
                                 // read 74.8 as 4.8 and eat a minus sign
        char* end = nullptr;
        const float f = strtof(v, &end);
        if (end != v) {
          out = f;
          err[0] = '\0';
          break;
        }
        k = strstr(v, "\"temperature_2m\":");
      }
    } else {
      snprintf(err, err_cap, "http status: %.16s", buf);
    }
  }
  net.stop();
  *dur_ms = millis() - t0;
  return out;
}

}  // namespace

bool enabled() { return WEATHER_LAT[0] != '\0' && WEATHER_LON[0] != '\0'; }

void tick() {
  if (!enabled() || WiFi.status() != WL_CONNECTED)
    return;
  const uint32_t due = g_ever ? kPollMs : kFirstPollDelayMs;
  if (g_last_ms != 0 && millis() - g_last_ms < due)
    return;
  if (g_last_ms == 0) {
    g_last_ms = millis();  // arm the first-poll delay from the first tick
    return;
  }
  g_last_ms = millis();
  g_ever = true;
  uint32_t dur = 0;
  char err[64];
  const float f = fetch_f(&dur, err, sizeof(err));
  // Same plausibility band as the MQTT outdoor path in mqtt_link.
  if (!isnan(f) && f > -60 && f < 150) {
    climate::set_outside_f(f);
    if (time_synced())
      climate::set_outdoor_epoch(time(nullptr));
    eventlog::log("wx", "%.1fF in %lums", f, (unsigned long)dur);
  } else {
    eventlog::log("wx", "FAILED after %lums: %s", (unsigned long)dur, err);
  }
  if (dur > 5000)
    eventlog::log("slow", "wx fetch %lums", (unsigned long)dur);
}

}  // namespace weather
