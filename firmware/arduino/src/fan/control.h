#pragma once
// The fan itself: the RMT waveform that replays the wall controller's PWM
// protocol, the current speed, and the auto-mode configuration around
// fan/auto_logic.h.
//
// Deliberately network-free. Anything that must happen when the speed changes
// (MQTT publish, SSE push) is a callback registered by main, so this module
// never links against the transport that announces it. The RMT peripheral
// keeps emitting the last waveform on its own hardware through WiFi loss and
// flash writes -- that isolation is the product's core safety property, and
// the module boundary mirrors it.

#include <Preferences.h>
#include <stdint.h>

namespace fan {

/** (new_speed, from_mqtt): announce a change however main sees fit. */
using Notify = void (*)(int speed, bool from_mqtt);
void set_notify(Notify cb);

/** Restore speed + auto config from NVS and resume the saved waveform. */
void restore(Preferences* prefs);

/**
 * Command a speed 0..12.
 * `source` is for the log line ("boot"/"http"/"mqtt"/"auto"); `manual` says a
 * human explicitly grabbed the wheel, which turns auto mode off. The caller
 * decides manual-ness -- retained MQTT replay right after connect is not
 * manual, and only the MQTT layer can know that.
 */
void apply(int v, const char* source, bool manual);

/** One 30 s auto-mode tick: refresh inside temp, run the thermostat. */
void tick_auto();

/** Drive a raw duty for calibration (/api/raw). Marks the speed as raw. */
void raw_high_us(uint16_t high_us);

/** Current speed 0..12, or negative while a raw duty is being driven. */
int speed();

/** Rough electrical draw at a given step (cubic fan law). */
float watts(int speed);

bool auto_on();
int auto_max();
int auto_min();
float engage_f();
float release_f();

/** Gas boost: VOC index >= gas_voc_on() forces at least gas_speed() in auto. */
bool gas_boost_on();
int gas_speed();
int gas_voc_on();
/** True while the VOC latch is holding the floor (for the console pill). */
bool gas_active();

void set_auto(bool on);
void set_gas_boost(bool on);
void set_gas_speed(int v);
void set_gas_voc_on(int v);
void set_auto_max(int v);
void set_auto_min(int v);
void set_engage_f(float v);
void set_release_f(float v);
/** Force release strictly below engage (persists if it had to move). */
void enforce_hysteresis_gap();

}  // namespace fan
