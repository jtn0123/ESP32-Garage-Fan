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
#include "net/mqtt_link.h"
#include "net/plug_cycle.h"
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

// The cycling profile. Fed every poll the verdict sees (and the ones it
// declines), because its whole value is noticing a pattern the per-poll
// verdict cannot: the fan stopping and restarting while one speed is held.
CycleDetector g_cycle;
// After onset, one trace line per poll for this many polls, so the tape
// carries the shape of the first five minutes (reading + the control pad as
// the chip sees it) and not just the headline.
constexpr uint8_t kTracePolls = 20;
uint8_t g_trace_left = 0;
// A heartbeat every 30 min while an episode lasts: "still cycling" with the
// running totals, so a long night reads as one story rather than a silence.
constexpr uint16_t kBeatPolls = 120;
uint16_t g_beat = 0;

/** The table speed whose baseline is nearest a reading -- "45 W is ~speed 12". */
int nearest_speed(float w) {
  int best = 0;
  for (int i = 1; i <= 12; i++) {
    if (fabsf(kBaselineW[i] - w) < fabsf(kBaselineW[best] - w))
      best = i;
  }
  return best;
}

/**
 * Read the control pad back and put it on the tape beside the meter's
 * testimony. This is the one line that separates "the chip stopped driving
 * the line" from "the fan stopped obeying it": a live pad shows ~84 % high
 * and six edges per 30 ms at speed 10; a dead one shows 0 or 100 and none.
 */
void log_pad(const char* tag) {
  float high_pct = 0;
  uint32_t edges = 0;
  fan::probe_pad(&high_pct, &edges);
  const uint16_t want = fan::commanded_high_us();
  eventlog::log("fan", "%s pad high=%.1f%% want=%.1f%% edges=%lu/30ms high_us=%u", tag,
                static_cast<double>(high_pct), 100.0 * want / kPeriodUs, (unsigned long)edges,
                want);
}

void publish_current_alert() {
  char alert[160];
  alert_json(alert, sizeof(alert));
  mqtt_link::publish_alert(alert);
}

/** What the cycling profile decided this poll, onto the tape and the broker. */
void on_cycle_event(CycleEvent ev, int speed) {
  if (ev == CycleEvent::kOnset) {
    eventlog::log("plug",
                  "CYCLING speed=%d flips=%u/10min now=%.1fW(~speed %d) -- the fan is "
                  "switching on and off while the speed is held",
                  speed, g_cycle.flips_in_window(), g_watts, nearest_speed(g_watts));
    log_pad("cycling");
    g_trace_left = kTracePolls;
    g_beat = 0;
    publish_current_alert();
    return;
  }
  if (ev == CycleEvent::kEnded) {
    const unsigned stopped_pct = g_cycle.polls ? 100u * g_cycle.stop_polls / g_cycle.polls : 0;
    eventlog::log(
        "plug", "cycling ended speed=%d after %u min flips=%u stopped=%u%% peak=%.1fW trough=%.1fW",
        speed, g_cycle.polls * (kPollMs / 1000) / 60, g_cycle.flips_total, stopped_pct,
        g_cycle.peak_w, g_cycle.trough_w >= kNoTrough ? 0.0f : g_cycle.trough_w);
    g_trace_left = 0;
    publish_current_alert();
    return;
  }
  if (!g_cycle.cycling)
    return;
  if (g_trace_left) {
    g_trace_left--;
    float high_pct = 0;
    uint32_t edges = 0;
    fan::probe_pad(&high_pct, &edges);
    eventlog::log("plug", "trace w=%.1f pad=%.1f%%/%lu", g_watts, static_cast<double>(high_pct),
                  (unsigned long)edges);
  }
  if (++g_beat >= kBeatPolls) {
    g_beat = 0;
    const unsigned stopped_pct = g_cycle.polls ? 100u * g_cycle.stop_polls / g_cycle.polls : 0;
    eventlog::log("plug",
                  "still cycling speed=%d %u min flips=%u stopped=%u%% peak=%.1fW trough=%.1fW",
                  speed, g_cycle.polls * (kPollMs / 1000) / 60, g_cycle.flips_total, stopped_pct,
                  g_cycle.peak_w, g_cycle.trough_w >= kNoTrough ? 0.0f : g_cycle.trough_w);
  }
}

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
  const bool in_table = speed >= 0 && speed <= 12;
  const bool fresh = g_ever_read && millis() - g_read_ms <= kStaleMs && !isnan(g_watts);
  const bool settled = millis() - g_speed_since_ms >= kSettleMs;
  // The cycling profile sees EVERY poll, decidable or not: its window is
  // wall-clock, and it applies the same settle and freshness gates itself.
  on_cycle_event(
      g_cycle.poll(speed, in_table ? kBaselineW[speed] : NAN, fresh ? g_watts : NAN, settled),
      speed);
  if (!fresh || !settled || !in_table) {
    // Falling from disagreement to CANNOT-SAY must replace the retained
    // alert too, or a stale "plug_disagree" keeps speaking for a meter that
    // stopped answering. It becomes "unknown", not "ok": silence is not
    // agreement.
    const bool was_disagree = g_verdict == -1;
    g_verdict = 0;
    if (was_disagree)
      publish_current_alert();
    return;
  }
  const float e = kBaselineW[speed];
  const float tol = e * 0.5f > 4.0f ? e * 0.5f : 4.0f;
  const bool in_band = fabsf(g_watts - e) <= tol;
  // Two consecutive out-of-band polls before crying wolf: a single reading
  // can straddle an auto-mode speed change this module has not seen yet.
  g_bad_streak = in_band ? 0 : g_bad_streak + 1;
  const int next = in_band ? 1 : (g_bad_streak >= 2 ? -1 : g_verdict);
  // Both transition edges go out as a retained MQTT alert as well as to the
  // flight recorder: the disagree case is "the belt broke" or "the connector
  // fell out" -- exactly the failure that ran the fan unnoticed for a day on
  // 2026-08-13 -- and a Home Assistant automation can only act on it if the
  // device says it out loud.
  //
  // Not while the cycling profile holds, though: a cycling fan flips this
  // verdict every minute or two, and 87 DISAGREE/agree lines in one night
  // (2026-08-20) said less than the one CYCLING line now does. The episode's
  // own onset/trace/heartbeat/ended lines and the plug_cycling alert carry
  // the story; the verdict keeps updating silently underneath.
  const bool quiet = g_cycle.cycling;
  if (next == -1 && g_verdict != -1) {
    g_verdict = next;
    if (!quiet) {
      eventlog::log("plug", "DISAGREE speed=%d expect=%.1fW measured=%.1fW", speed, e, g_watts);
      publish_current_alert();
    }
  } else if (next == 1 && g_verdict == -1) {
    g_verdict = next;
    if (!quiet) {
      eventlog::log("plug", "agree again speed=%d measured=%.1fW", speed, g_watts);
      publish_current_alert();
    }
  }
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

bool cycling() { return g_cycle.cycling; }
uint8_t flips() { return g_cycle.flips_in_window(); }
int8_t take_bucket_flips() {
  if (!enabled() || !g_ever_read)
    return -1;
  const uint8_t n = g_cycle.take_bucket_flips();
  return static_cast<int8_t>(n > 127 ? 127 : n);
}

void alert_json(char* out, size_t cap) {
  const int speed = fan::speed();
  if (g_cycle.cycling) {
    // Outranks the verdict: "cycling" is the more specific finding, and an
    // automation that only knew plug_disagree would see it flap every minute.
    snprintf(out, cap,
             "{\"kind\":\"plug_cycling\",\"speed\":%d,\"flips\":%u,\"peak_w\":%.1f,"
             "\"trough_w\":%.1f}",
             speed, g_cycle.flips_in_window(), g_cycle.peak_w,
             g_cycle.trough_w >= kNoTrough ? 0.0f : g_cycle.trough_w);
    return;
  }
  if (g_verdict == -1 && speed >= 0 && speed <= 12) {
    snprintf(out, cap,
             "{\"kind\":\"plug_disagree\",\"speed\":%d,\"expect_w\":%.1f,"
             "\"measured_w\":%.1f}",
             speed, kBaselineW[speed], g_ever_read ? g_watts : -1.0f);
  } else if (g_verdict == 1) {
    snprintf(out, cap, "{\"kind\":\"ok\"}");
  } else {
    // Verdict 0: stale reading, speed still settling, or no meter. Distinct
    // from ok so an automation can tell "verified fine" from "cannot say".
    snprintf(out, cap, "{\"kind\":\"unknown\"}");
  }
}

}  // namespace plug
