// Controller writes, kept separate so the real handlers run in host tests.
#include "net/web_controls.h"

#include <Arduino.h>
#include <cstdio>

#include "config.h"
#include "fan/control.h"
#include "net/web_gate.h"
#include "net/numeric_arg.h"
#include "system/odometer.h"

namespace web_controls {
namespace {
WebServer* g_http = nullptr;
Preferences* g_prefs = nullptr;
char* g_token = nullptr;
size_t g_token_cap = 0;
StateReply g_state = nullptr;
StateReply g_push = nullptr;
bool guard_origin() { return web_gate::guard_origin(*g_http); }
bool bad_argument(const char* name) {
  char body[64];
  snprintf(body, sizeof(body), "{\"error\":\"bad %s\"}", name);
  g_http->send(400, "application/json", body);
  return false;
}

static void handle_config() {
  if (!guard_origin())
    return;
  struct IntField {
    const char* name;
    long low;
    long high;
    void (*set)(int);
  };
  const IntField ints[] = {{"gasspd", 1, 12, fan::set_gas_speed},
                           {"gasvoc", 100, 500, fan::set_gas_voc_on},
                           {"max", 1, 12, fan::set_auto_max},
                           {"min", 0, 12, fan::set_auto_min}};
  struct RealField {
    const char* name;
    float low;
    float high;
    void (*set)(float);
  };
  const RealField reals[] = {{"ckwh", 0.01f, 2, odometer::set_cost_per_kwh},
                             {"onf", 0.5f, 20, fan::set_engage_f},
                             {"offf", 0, 20, fan::set_release_f},
                             {"floorf", 32, 100, fan::set_floor_f},
                             {"startf", 32, 120, fan::set_start_f}};
  struct BoolField {
    const char* name;
    void (*set)(bool);
  };
  const BoolField bools[] = {{"auto", fan::set_auto},
                             {"gason", fan::set_gas_boost},
                             {"flooron", fan::set_floor_on},
                             {"starton", fan::set_start_on}};
  constexpr size_t kInts = sizeof(ints) / sizeof(ints[0]);
  constexpr size_t kReals = sizeof(reals) / sizeof(reals[0]);
  constexpr size_t kBools = sizeof(bools) / sizeof(bools[0]);
  long iv[kInts] = {}, bv[kBools] = {};
  float rv[kReals] = {};
  // Validate every supplied field before any setter can persist a change.
  for (size_t i = 0; i < kInts; ++i) {
    const auto& f = ints[i];
    if (g_http->hasArg(f.name) &&
        !numeric_arg::integer(g_http->arg(f.name).c_str(), f.low, f.high, &iv[i])) {
      bad_argument(f.name);
      return;
    }
  }
  for (size_t i = 0; i < kReals; ++i) {
    const auto& f = reals[i];
    if (g_http->hasArg(f.name) &&
        !numeric_arg::real(g_http->arg(f.name).c_str(), f.low, f.high, &rv[i])) {
      bad_argument(f.name);
      return;
    }
  }
  for (size_t i = 0; i < kBools; ++i) {
    const auto& f = bools[i];
    if (g_http->hasArg(f.name) &&
        !numeric_arg::integer(g_http->arg(f.name).c_str(), 0, 1, &bv[i])) {
      bad_argument(f.name);
      return;
    }
  }
  if (g_http->hasArg("newtoken")) {
    if (!web_gate::guard_token(*g_http, g_token))
      return;
    const String next = g_http->arg("newtoken");
    if (!token_policy::usable(next.c_str())) {
      bad_argument("newtoken");
      return;
    }
    if (!g_prefs || g_prefs->putString("token", next) != next.length()) {
      g_http->send(503, "application/json", "{\"error\":\"token could not be saved\"}");
      return;
    }
    snprintf(g_token, g_token_cap, "%s", next.c_str());
  }
  for (size_t i = 0; i < kBools; ++i)
    if (g_http->hasArg(bools[i].name))
      bools[i].set(bv[i] != 0);
  for (size_t i = 0; i < kInts; ++i)
    if (g_http->hasArg(ints[i].name))
      ints[i].set(static_cast<int>(iv[i]));
  for (size_t i = 0; i < kReals; ++i)
    if (g_http->hasArg(reals[i].name))
      reals[i].set(rv[i]);
  fan::enforce_hysteresis_gap();
  g_push();
  g_state();
}

// Calibration instrument: drive an arbitrary duty, no reflash per data point.
static void handle_raw() {
  if (!guard_origin())
    return;

  if (!g_http->hasArg("high_pct")) {
    g_http->send(400, "application/json", "{\"error\":\"high_pct 0-100\"}");
    return;
  }
  long pct = 0;
  if (!numeric_arg::integer(g_http->arg("high_pct").c_str(), 0, 100, &pct)) {
    g_http->send(400, "application/json", "{\"error\":\"high_pct 0-100\"}");
    return;
  }
  if (!fan::raw_high_us(static_cast<uint16_t>(static_cast<uint32_t>(kPeriodUs) * pct / 100))) {
    g_http->send(503, "application/json", "{\"error\":\"PWM command failed\"}");
    return;
  }
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"raw_high_pct\":%ld}", pct);
  g_http->send(200, "application/json", buf);
}

static void handle_set() {
  if (!guard_origin())
    return;

  if (!g_http->hasArg("speed")) {
    g_http->send(400, "application/json", "{\"error\":\"speed required\"}");
    return;
  }
  long v = 0;
  if (!numeric_arg::integer(g_http->arg("speed").c_str(), 0, 12, &v)) {
    g_http->send(400, "application/json", "{\"error\":\"0-12 only\"}");
    return;
  }
  if (!fan::apply(static_cast<int>(v), "http", /*manual=*/true)) {
    g_http->send(503, "application/json", "{\"error\":\"PWM command failed\"}");
    return;
  }
  g_state();
}

}  // namespace

void register_routes(WebServer& http, Preferences* prefs, char (&token)[40], StateReply state,
                     StateReply push) {
  g_http = &http;
  g_prefs = prefs;
  g_token = token;
  g_token_cap = sizeof(token);
  g_state = state;
  g_push = push;
  http.on("/api/set", HTTP_POST, handle_set);
  http.on("/api/raw", HTTP_POST, handle_raw);
  http.on("/api/config", HTTP_POST, handle_config);
}
}  // namespace web_controls
