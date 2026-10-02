// See control.h. The waveform construction is verbatim from the pre-split
// firmware; PROTOCOL.md is ground truth for the duty table it consumes.
#include "fan/control.h"

#include <Arduino.h>

#include <cmath>
#include <cstring>

#include "config.h"
#include "fan/auto_logic.h"
#include "soc/gpio_periph.h"
#include "system/eventlog.h"
#include "sensors/air.h"
#include "sensors/climate.h"

namespace fan {
namespace {

Preferences* g_prefs = nullptr;
Notify g_notify = nullptr;
bool g_ledc_ready = false;
bool g_output_fault = false;
uint16_t g_high_us = 0;  // what set_wave last drove, for probe_pad's "want"
int g_speed = 0;
bool g_auto_on = false;
int g_auto_max = 9;
int g_auto_min = 0;                           // rest speed once equalized (0 = off)
float g_auto_onf = 2.5f, g_auto_offf = 1.5f;  // engage/release, deg F
bool g_auto_high = false;                     // hysteresis latch for fan_auto_decide
bool g_gas_on = kFanGasDefaults.enabled;      // gas boost: VOC index forces a floor
int g_gas_spd = kFanGasDefaults.boost_speed;
int g_gas_voc = kFanGasDefaults.on_index;
bool g_gas_high = false;  // hysteresis latch for fan_gas_floor
// Absolute limits (auto_logic.h): stored values survive the switch being off,
// so flipping winter on and off does not lose the temperatures.
bool g_floor_on = false;
float g_floor_f = 64.0f;
bool g_start_on = false;
float g_start_f = 74.0f;
FanLimit g_limit = FanLimit::kNone;  // what the last tick reported, for the console

// Minimum-run dwell (auto_logic.h owns the why and the 15-min sizing): once
// a latch engages it may not release for this many 30 s ticks. Engage stays
// instant; only early surrender waits.
constexpr uint16_t kMinRunTicks = 15 * 60 * 1000 / kAutoTickMs;
uint16_t g_auto_run_ticks = 0;  // ticks the thermostat latch has been high
uint16_t g_gas_run_ticks = 0;   // ticks the gas latch has been high

// One auto-mode telemetry line per 5 minutes of ticks (see tick_auto).
constexpr uint8_t kAutoLogTicks = 5 * 60 * 1000 / kAutoTickMs;
uint8_t g_auto_log_ticks = 0;

// LEDC, not RMT. On 2026-08-13 the fan ignored an entire 0..12 calibration
// sweep against a watt meter: rmtWriteLooping() reported success at every
// step while /api/pinprobe showed the pad stuck LOW with zero transitions --
// the waveform never left the chip. LEDC is the peripheral PROTOCOL.md's
// replication plan called for in the first place: hardware-looped, and its
// output is verifiable on the pad.
//
// 12-bit at 100 Hz vs the captured 100.66 Hz: duty FRACTIONS are preserved
// exactly (both edges scale by the same 0.66%), and the fan demonstrably
// low-passes duty rather than counting edges. Failures still go to the
// flight recorder, never Serial -- a deployed S2's CDC drops prints.
constexpr uint8_t kLedcBits = 12;

bool set_wave(uint16_t high_us) {
  if (!g_ledc_ready) {
    if (!ledcAttach(FAN_PWM_PIN, 1000000UL / kPeriodUs, kLedcBits)) {
      eventlog::log("fan", "ledcAttach FAILED pin %d", FAN_PWM_PIN);
      g_output_fault = true;
      return false;
    }
    g_ledc_ready = true;
  }
  // duty 0 = solid LOW; duty 2^bits = solid HIGH, no one-tick glitch.
  uint32_t duty;
  if (high_us == 0)
    duty = 0;
  else if (high_us >= kPeriodUs)
    duty = 1UL << kLedcBits;
  else
    duty = (static_cast<uint32_t>(high_us) * ((1UL << kLedcBits) - 1)) / kPeriodUs;
  if (!ledcWrite(FAN_PWM_PIN, duty)) {
    eventlog::log("fan", "ledcWrite FAILED duty=%lu", (unsigned long)duty);
    g_output_fault = true;
    return false;
  }
  g_high_us = high_us;
  g_output_fault = false;
  // Read the pad back and put it on the SAME line, so every speed change
  // carries its own proof that the waveform left the chip. ledcWrite applies
  // the new duty at the next period boundary, hence the one-period wait --
  // measuring immediately would blend the old duty into the answer and the
  // proof would be worth less than nothing.
  delay(kPeriodUs / 1000 + 2);
  float pad_pct = 0;
  uint32_t edges = 0;
  probe_pad(&pad_pct, &edges);
  eventlog::log("fan", "duty %lu/4096 high_us=%u pad=%.0f%%/%lu", (unsigned long)duty, high_us,
                static_cast<double>(pad_pct), (unsigned long)edges);
  return true;
}

}  // namespace

void set_notify(Notify cb) { g_notify = cb; }

void restore(Preferences* prefs) {
  g_prefs = prefs;
  g_speed = 0;
  if (prefs) {
    g_auto_on = prefs->getBool("auto", false);
    g_auto_max = prefs->getInt("max", 9);
    g_auto_min = prefs->getInt("amin", 0);
    g_auto_onf = prefs->getFloat("onf", 2.5f);
    g_auto_offf = prefs->getFloat("offf", 1.5f);
    g_gas_on = prefs->getBool("gason", kFanGasDefaults.enabled);
    g_gas_spd = prefs->getInt("gasspd", kFanGasDefaults.boost_speed);
    g_gas_voc = prefs->getInt("gasvoc", kFanGasDefaults.on_index);
    g_floor_on = prefs->getBool("flooron", false);
    g_floor_f = prefs->getFloat("floorf", 64.0f);
    g_start_on = prefs->getBool("starton", false);
    g_start_f = prefs->getFloat("startf", 74.0f);
    const int saved = prefs->getInt("speed", 0);
    if (saved > 0 && saved <= 12) {
      g_speed = saved;
      Serial.printf("restored speed %d from nvs\n", saved);
    }
  }
  // Drive the line UNCONDITIONALLY, speed 0 included. The old guard only
  // touched the pin for saved > 0, so a reboot at speed 0 left the GPIO
  // floating -- never even rmtInit'd -- and the fan's own pull-up read as
  // full power on this rig while the panel and console both said OFF, and
  // auto could never fix it because apply(0) == g_speed short-circuits
  // (observed live 2026-08-13). An undriven control line is not "off"; it is
  // "whatever the fan feels like".
  const bool driven = set_wave(kHighUs[g_speed]);  // resume before WiFi
  if (!driven)
    g_speed = -2;  // output unknown; do not claim the saved speed was applied
}

// source: "boot", "http", "mqtt", "auto" -- log flavour only; `manual` is the
// decision (the human explicitly grabbed the wheel, so auto lets go).
bool apply(int v, const char* source, bool manual) {
  if (v < 0 || v > 12)
    return false;
  const bool changed = v != g_speed || g_output_fault;
  if (changed && !set_wave(kHighUs[v]))
    return false;
  // Manual override first: selecting the speed the fan already runs at is
  // still the human grabbing the wheel, so auto lets go even though the
  // waveform does not change.
  if (manual && g_auto_on) {
    g_auto_on = false;
    if (g_prefs)
      g_prefs->putBool("auto", false);
    Serial.println("auto mode off (manual override)");
  }
  if (!changed)
    return true;
  g_speed = v;
  if (g_prefs)
    g_prefs->putInt("speed", v);  // survives power loss even broker-less
  if (g_notify)
    g_notify(v, strcmp(source, "mqtt") == 0);
  Serial.printf("speed -> %d via %s\n", v, source);
  return true;
}

void tick_auto() {
  if (!g_auto_on)
    return;
  climate::refresh_inside();
  FanAutoCfg cfg = kFanAutoDefaults;
  cfg.min_speed = g_auto_min;
  cfg.max_speed = g_auto_max;
  cfg.on_delta_c = g_auto_onf * 5 / 9;  // user thinks in F; logic runs in C
  cfg.off_delta_c = g_auto_offf * 5 / 9;
  cfg.floor_c = g_floor_on ? (g_floor_f - 32) * 5 / 9 : kLimitOff;
  cfg.start_c = g_start_on ? (g_start_f - 32) * 5 / 9 : kLimitOff;
  const int prev = g_speed < 0 ? 0 : g_speed;
  const float inside = climate::inside_c();
  const float outside = climate::outside_c_fresh();
  const bool was_running = g_auto_high;
  int next =
      fan_auto_decide(inside, outside, prev, &g_auto_high, cfg, &g_auto_run_ticks, kMinRunTicks);
  g_limit = fan_auto_limit(inside, outside, g_auto_high, cfg);
  // The one release that skips the dwell gets its own line: "why did the fan
  // quit after four minutes" should not need the thresholds reconstructed.
  // Not tagged "auto" -- fantape reads every auto line as the periodic one.
  if (was_running && !g_auto_high && g_limit == FanLimit::kFloor)
    eventlog::log("limit", "low limit %.1fF reached at %.1fF: resting",
                  static_cast<double>(g_floor_f), static_cast<double>(inside * 9 / 5 + 32));
  // The gas floor layers under the thermostat: bad air forces a minimum
  // speed, it never lowers what the thermostat wanted. The release edge of
  // the latch goes to the flight recorder so "why did the fan spin up at
  // 2am" has an answer.
  FanGasCfg gcfg = kFanGasDefaults;
  gcfg.enabled = g_gas_on;
  gcfg.boost_speed = g_gas_spd;
  gcfg.on_index = g_gas_voc;
  gcfg.off_index = g_gas_voc - kGasReleaseGap > 0 ? g_gas_voc - kGasReleaseGap : 1;
  const bool was_high = g_gas_high;
  const int floor_speed = fan_gas_floor(static_cast<int>(air::voc_index()), &g_gas_high, gcfg,
                                        &g_gas_run_ticks, kMinRunTicks);
  if (g_gas_high != was_high)
    eventlog::log("gas", "boost %s voc=%ld floor=%d", g_gas_high ? "ON" : "off",
                  (long)air::voc_index(), floor_speed);
  next = fan_apply_gas_floor(next, prev, floor_speed);
  // One telemetry line every 5 minutes (10 ticks). The tape has always
  // recorded WHAT the thermostat did and never WHY, and the dwell counter --
  // which decides whether a release is honoured at all -- appeared nowhere.
  // Rendered by auto_logic.h so the host tests can pin the wording and the
  // 80-byte budget.
  if (++g_auto_log_ticks >= kAutoLogTicks) {
    g_auto_log_ticks = 0;
    char msg[80];
    fan_auto_log_line(msg, sizeof(msg), inside, outside, g_auto_high, g_auto_run_ticks,
                      kMinRunTicks, g_auto_high ? cfg.max_speed : cfg.min_speed, g_gas_high,
                      g_limit);
    eventlog::log("auto", "%s", msg);
  }
  if (next != g_speed || g_output_fault)
    apply(next, "auto", false);
}

bool raw_high_us(uint16_t high_us) {
  if (!set_wave(high_us))
    return false;
  g_speed = -1;  // sentinel: /api/set?speed=0 must force a real re-apply
  return true;
}

bool output_fault() { return g_output_fault; }

int speed() { return g_speed; }

uint16_t commanded_high_us() { return g_high_us; }

void probe_pad(float* high_pct, uint32_t* transitions) {
  // Input buffer ONLY, straight at the IO_MUX. gpio_set_direction() looked
  // right and was a trap: it reroutes the pad's output select to simple
  // GPIO, silently disconnecting LEDC/RMT -- the probe then reports the
  // stuck-low line the probe itself just created.
  REG_SET_BIT(GPIO_PIN_MUX_REG[FAN_PWM_PIN], FUN_IE);
  uint32_t edges = 0;
  uint32_t highs = 0;
  uint32_t n = 0;
  int last = digitalRead(FAN_PWM_PIN);
  const uint32_t t0 = micros();
  while (micros() - t0 < 30000) {
    const int v = digitalRead(FAN_PWM_PIN);
    highs += v;
    ++n;
    if (v != last) {
      ++edges;
      last = v;
    }
  }
  if (high_pct)
    *high_pct = n ? 100.0f * highs / n : 0.0f;
  if (transitions)
    *transitions = edges;
}

float watts(int speed) {
  if (speed <= 0)
    return 0;
  const float f = speed / 12.0f;
  return 5.0f + 100.0f * f * f * f;  // cubic fan law, rough estimate
}

bool auto_on() { return g_auto_on; }
bool gas_boost_on() { return g_gas_on; }
int gas_speed() { return g_gas_spd; }
int gas_voc_on() { return g_gas_voc; }
bool gas_active() { return g_gas_high; }
bool floor_on() { return g_floor_on; }
float floor_f() { return g_floor_f; }
bool start_on() { return g_start_on; }
float start_f() { return g_start_f; }
// Manual mode has no thermostat, so nothing is being overridden.
const char* limit() { return g_auto_on ? fan_limit_name(g_limit) : nullptr; }
int auto_max() { return g_auto_max; }
int auto_min() { return g_auto_min; }
float engage_f() { return g_auto_onf; }
float release_f() { return g_auto_offf; }

void set_auto(bool on) {
  g_auto_on = on;
  if (g_prefs)
    g_prefs->putBool("auto", on);
  Serial.printf("auto mode %s\n", on ? "on" : "off");
}

void set_gas_boost(bool on) {
  g_gas_on = on;
  if (!on) {
    g_gas_high = false;  // releasing the feature releases the latch too
    g_gas_run_ticks = 0;
  }
  if (g_prefs)
    g_prefs->putBool("gason", on);
}

void set_gas_speed(int v) {
  g_gas_spd = v;
  if (g_prefs)
    g_prefs->putInt("gasspd", v);
}

void set_gas_voc_on(int v) {
  g_gas_voc = v;
  if (g_prefs)
    g_prefs->putInt("gasvoc", v);
}

void set_auto_max(int v) {
  g_auto_max = v;
  if (g_prefs)
    g_prefs->putInt("max", v);
}

void set_auto_min(int v) {
  g_auto_min = v;
  if (g_prefs)
    g_prefs->putInt("amin", v);
}

void set_engage_f(float v) {
  g_auto_onf = v;
  if (g_prefs)
    g_prefs->putFloat("onf", v);
}

void set_release_f(float v) {
  g_auto_offf = v;
  if (g_prefs)
    g_prefs->putFloat("offf", v);
}

// Switching a limit changes what the last tick's verdict meant, so it is
// cleared rather than left to claim a limit that no longer exists; the next
// tick (30 s) reports afresh.
void set_floor_on(bool on) {
  g_floor_on = on;
  g_limit = FanLimit::kNone;
  if (g_prefs)
    g_prefs->putBool("flooron", on);
}

void set_floor_f(float v) {
  g_floor_f = v;
  g_limit = FanLimit::kNone;
  if (g_prefs)
    g_prefs->putFloat("floorf", v);
}

void set_start_on(bool on) {
  g_start_on = on;
  g_limit = FanLimit::kNone;
  if (g_prefs)
    g_prefs->putBool("starton", on);
}

void set_start_f(float v) {
  g_start_f = v;
  g_limit = FanLimit::kNone;
  if (g_prefs)
    g_prefs->putFloat("startf", v);
}

void enforce_hysteresis_gap() {
  // Hysteresis needs release strictly below engage or the latch flaps.
  if (g_auto_offf >= g_auto_onf) {
    g_auto_offf = g_auto_onf > 0.5f ? g_auto_onf - 0.5f : 0;
    if (g_prefs)
      g_prefs->putFloat("offf", g_auto_offf);
  }
}

}  // namespace fan
