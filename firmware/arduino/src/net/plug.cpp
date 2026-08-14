// See plug.h. Two small GETs against Home Assistant's REST API, parsed with a
// bare string scan like net/weather.cpp -- the only field wanted from each is
// "state".
#include "net/plug.h"

#include <Arduino.h>
#include <WiFi.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "fan/control.h"
#include "generated_config.h"
#include "system/eventlog.h"

namespace plug {
namespace {

// Measured 2026-08-14 with the lag-hardened sweep in
// scripts/calibrate_fan_power.py (docs/fan_power_baseline.json). The plug's
// Matter sensor serves the previous speed's watts for ~a minute, which had
// shifted the whole 2026-08-13 table one speed to the right; this table
// waited out that lag at every step and cross-checks against two
// independently dwelled spot measurements (7 and 8) within 0.1 W.
// clang-format off
constexpr float kBaselineW[13] = {1.4f, 3.7f, 4.8f, 7.0f, 7.6f, 10.0f, 12.5f,  // NOLINT
                                  15.8f, 19.7f, 25.0f, 30.2f, 37.7f, 44.2f};   // NOLINT
// clang-format on

constexpr uint32_t kPollMs = 15000;
constexpr uint32_t kFirstPollDelayMs = 25000;  // let WiFi/MQTT/SD settle first
constexpr uint32_t kStaleMs = 60000;
/** A verdict needs the speed to have been steady this long: spin-up and
 *  spin-down take seconds, and the plug's sensor lags tens more. */
constexpr uint32_t kSettleMs = 90000;

uint32_t g_last_poll_ms = 0;
bool g_ever_polled = false;
float g_watts = NAN;
float g_volts = NAN;
uint32_t g_read_ms = 0;
bool g_ever_read = false;
int g_speed_seen = -99;
uint32_t g_speed_since_ms = 0;
int g_verdict = 0;
int g_bad_streak = 0;
uint8_t g_fetch_fails = 0;  // consecutive failed polls; the module that exists
                            // to catch silent faults must not fail silently

/** GET one entity's "state" as a float; NAN on any failure. */
float fetch_state(const char* entity) {
  // HA_URL is baked as e.g. "http://10.27.27.27:8123"; split host and port.
  const char* url = HA_URL;
  if (strncmp(url, "http://", 7) == 0)
    url += 7;
  char host[64];
  uint16_t port = 8123;
  const char* colon = strchr(url, ':');
  if (colon) {
    const size_t hl = colon - url < 63 ? static_cast<size_t>(colon - url) : 63;
    memcpy(host, url, hl);
    host[hl] = '\0';
    port = static_cast<uint16_t>(atoi(colon + 1));
  } else {
    snprintf(host, sizeof(host), "%s", url);
  }

  WiFiClient net;
  net.setTimeout(5000);
  if (!net.connect(host, port))
    return NAN;
  // The token rides as a %s ARGUMENT: pasted into the format string, a '%'
  // inside it would become a conversion specifier reading arguments that
  // were never passed.
  net.printf(
      "GET /api/states/%s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\n"
      "Connection: close\r\n\r\n",
      entity, host, HA_TOKEN);
  char buf[1024];
  size_t n = 0;
  const uint32_t t0 = millis();
  while (n < sizeof(buf) - 1 && millis() - t0 < 5000) {
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
  if (strncmp(buf, "HTTP/1.1 200", 12) != 0)
    return NAN;
  const char* k = strstr(buf, "\"state\":\"");
  if (!k)
    return NAN;
  char* end = nullptr;
  const float v = strtof(k + 9, &end);
  // "unavailable"/"unknown" parse to 0 with end==start; reject those.
  return (end && end != k + 9) ? v : NAN;
}

void judge() {
  // Track how long the commanded speed has been steady.
  const int speed = fan::speed();
  if (speed != g_speed_seen) {
    g_speed_seen = speed;
    g_speed_since_ms = millis();
    g_bad_streak = 0;
  }
  if (!g_ever_read || millis() - g_read_ms > kStaleMs || millis() - g_speed_since_ms < kSettleMs ||
      speed < 0 || speed > 12 || isnan(g_watts)) {
    g_verdict = 0;
    return;
  }
  const float e = kBaselineW[speed];
  const float tol = e * 0.5f > 4.0f ? e * 0.5f : 4.0f;
  const bool in_band = fabsf(g_watts - e) <= tol;
  // Two consecutive out-of-band polls before crying wolf: a single reading
  // can straddle an auto-mode speed change this module has not seen yet.
  g_bad_streak = in_band ? 0 : g_bad_streak + 1;
  const int next = in_band ? 1 : (g_bad_streak >= 2 ? -1 : g_verdict);
  if (next == -1 && g_verdict != -1)
    eventlog::log("plug", "DISAGREE speed=%d expect=%.1fW measured=%.1fW", speed, e, g_watts);
  if (next == 1 && g_verdict == -1)
    eventlog::log("plug", "agree again speed=%d measured=%.1fW", speed, g_watts);
  g_verdict = next;
}

}  // namespace

bool enabled() { return HA_URL[0] != '\0' && HA_TOKEN[0] != '\0'; }

void tick() {
  if (!enabled() || WiFi.status() != WL_CONNECTED)
    return;
  const uint32_t now = millis();
  if (g_ever_polled ? now - g_last_poll_ms < kPollMs : now < kFirstPollDelayMs)
    return;
  g_last_poll_ms = now;
  g_ever_polled = true;
  const float w = fetch_state(FAN_PLUG_POWER_ENTITY);
  if (!isnan(w)) {
    if (g_fetch_fails >= 4)
      eventlog::log("plug", "HA reads resumed after %u failed polls", g_fetch_fails);
    g_fetch_fails = 0;
    g_watts = w;
    g_volts = fetch_state(FAN_PLUG_VOLT_ENTITY);
    g_read_ms = millis();
    g_ever_read = true;
  } else if (++g_fetch_fails == 4) {
    // One line at the threshold, not one per poll: enough to name the fault
    // without turning a dead HA into a log flood.
    eventlog::log("plug", "cannot read the watt meter from HA (4 consecutive polls)");
  }
  judge();
}

float watts() { return g_ever_read ? g_watts : NAN; }
float volts() { return g_ever_read ? g_volts : NAN; }

int32_t age_s() {
  if (!g_ever_read)
    return -1;
  return static_cast<int32_t>((millis() - g_read_ms) / 1000);
}

int verdict() { return g_verdict; }

float expected_w(int speed) { return (speed >= 0 && speed <= 12) ? kBaselineW[speed] : NAN; }

}  // namespace plug
