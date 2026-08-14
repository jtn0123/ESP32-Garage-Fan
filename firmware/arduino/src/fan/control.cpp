// See control.h. The waveform construction is verbatim from the pre-split
// firmware; PROTOCOL.md is ground truth for the duty table it consumes.
#include "fan/control.h"

#include <Arduino.h>

#include <cmath>
#include <cstring>

#include "config.h"
#include "fan/auto_logic.h"
#include "sensors/climate.h"

namespace fan {
namespace {

Preferences* g_prefs = nullptr;
Notify g_notify = nullptr;
bool g_rmt_ready = false;
rmt_data_t g_wave;
int g_speed = 0;
bool g_auto_on = false;
int g_auto_max = 9;
int g_auto_min = 0;                           // rest speed once equalized (0 = off)
float g_auto_onf = 2.5f, g_auto_offf = 1.5f;  // engage/release, deg F
bool g_auto_high = false;                     // hysteresis latch for fan_auto_decide

void set_wave(uint16_t high_us) {
  if (!g_rmt_ready) {
    if (!rmtInit(FAN_PWM_PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 1000000)) {
      Serial.println("rmtInit FAILED");
      return;
    }
    g_rmt_ready = true;
  }
  if (high_us >= kPeriodUs || high_us == 0) {
    g_wave.level0 = high_us ? 1 : 0;
    g_wave.duration0 = kPeriodUs / 2;
    g_wave.level1 = high_us ? 1 : 0;
    g_wave.duration1 = kPeriodUs - kPeriodUs / 2;
  } else {
    g_wave.level0 = 1;
    g_wave.duration0 = high_us;
    g_wave.level1 = 0;
    g_wave.duration1 = kPeriodUs - high_us;
  }
  rmtWriteLooping(FAN_PWM_PIN, &g_wave, 1);
}

}  // namespace

void set_notify(Notify cb) { g_notify = cb; }

void restore(Preferences* prefs) {
  g_prefs = prefs;
  if (prefs) {
    g_auto_on = prefs->getBool("auto", false);
    g_auto_max = prefs->getInt("max", 9);
    g_auto_min = prefs->getInt("amin", 0);
    g_auto_onf = prefs->getFloat("onf", 2.5f);
    g_auto_offf = prefs->getFloat("offf", 1.5f);
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
  set_wave(kHighUs[g_speed]);  // resume before WiFi even exists
}

// source: "boot", "http", "mqtt", "auto" -- log flavour only; `manual` is the
// decision (the human explicitly grabbed the wheel, so auto lets go).
void apply(int v, const char* source, bool manual) {
  if (v < 0 || v > 12)
    return;
  // Manual override first: selecting the speed the fan already runs at is
  // still the human grabbing the wheel, so auto lets go even though the
  // waveform does not change.
  if (manual && g_auto_on) {
    g_auto_on = false;
    if (g_prefs)
      g_prefs->putBool("auto", false);
    Serial.println("auto mode off (manual override)");
  }
  if (v == g_speed)
    return;
  g_speed = v;
  set_wave(kHighUs[v]);
  if (g_prefs)
    g_prefs->putInt("speed", v);  // survives power loss even broker-less
  if (g_notify)
    g_notify(v, strcmp(source, "mqtt") == 0);
  Serial.printf("speed -> %d via %s\n", v, source);
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
  const int next = fan_auto_decide(climate::inside_c(), climate::outside_c_fresh(),
                                   g_speed < 0 ? 0 : g_speed, &g_auto_high, cfg);
  if (next != g_speed)
    apply(next, "auto", false);
}

void raw_high_us(uint16_t high_us) {
  set_wave(high_us);
  g_speed = -1;  // sentinel: /api/set?speed=0 must force a real re-apply
}

int speed() { return g_speed; }

float watts(int speed) {
  if (speed <= 0)
    return 0;
  const float f = speed / 12.0f;
  return 5.0f + 100.0f * f * f * f;  // cubic fan law, rough estimate
}

bool auto_on() { return g_auto_on; }
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

void enforce_hysteresis_gap() {
  // Hysteresis needs release strictly below engage or the latch flaps.
  if (g_auto_offf >= g_auto_onf) {
    g_auto_offf = g_auto_onf > 0.5f ? g_auto_onf - 0.5f : 0;
    if (g_prefs)
      g_prefs->putFloat("offf", g_auto_offf);
  }
}

}  // namespace fan
