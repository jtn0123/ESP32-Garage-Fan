// See wifi_link.h.
#include "net/wifi_link.h"

#include <Arduino.h>
#include <WiFi.h>

#include <cstdlib>
#include <ctime>

#include "config.h"
#include "net/creds.h"
#include "system/eventlog.h"

namespace wifi_link {
namespace {

uint32_t g_drops = 0;
uint32_t g_down_since_ms = 0;  // 0 while connected; a real ms (never 0) while down
uint32_t g_last_nudge_ms = 0;

constexpr uint32_t kNudgeMs = 60 * 1000;
constexpr uint32_t kRebootMs = 15 * 60 * 1000;

// The disconnect reasons this rig plausibly meets, named so the event log
// reads without the IEEE 802.11 table open. Anything else logs numerically.
const char* reason_str(uint8_t r) {
  switch (r) {
    case 2:
      return "auth_expire";
    case 4:
      return "assoc_expire";
    case 8:
      return "leaving";
    case 15:
      return "4way_timeout";
    case 200:
      return "beacon_timeout";
    case 201:
      return "no_ap_found";
    case 202:
      return "auth_fail";
    case 203:
      return "assoc_fail";
    case 204:
      return "handshake_timeout";
    default:
      return "?";
  }
}

// Runs on the network event task, not loop() -- eventlog is the one safe
// cross-task sink here (its ring is spinlocked); keep everything else out.
void on_event(WiFiEvent_t ev, WiFiEventInfo_t info) {
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      eventlog::log("wifi", "up ip=%s rssi=%d", WiFi.localIP().toString().c_str(),
                    static_cast<int>(WiFi.RSSI()));
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      g_drops++;
      eventlog::log("wifi", "drop reason=%u(%s) n=%lu",
                    static_cast<unsigned>(info.wifi_sta_disconnected.reason),
                    reason_str(info.wifi_sta_disconnected.reason), (unsigned long)g_drops);
      break;
    default:
      break;
  }
}

uint32_t now_nonzero() {
  const uint32_t ms = millis();
  return ms ? ms : 1;
}

}  // namespace

void begin() {
  WiFi.onEvent(on_event);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  // The core default, but this module's whole job is self-healing, so the
  // first rung of the ladder does not get to depend on a default.
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(FAN_HOSTNAME);
  WiFi.begin(creds::wifi_ssid(), creds::wifi_pass());  // NVS-backed, see creds.h
  // Clock stays UTC: every epoch this firmware logs, publishes or compares is
  // UTC and must remain so. TZ is set alongside it purely so localtime_r() is
  // available to the one caller that needs the operator's calendar day --
  // odometer's "fan today" reset. Setting TZ does not move the clock.
  configTime(0, 0, "pool.ntp.org");
  setenv("TZ", FAN_TZ, 1);
  tzset();
  g_down_since_ms = now_nonzero();
}

void tick() {
  if (WiFi.status() == WL_CONNECTED) {
    g_down_since_ms = 0;
    return;
  }
  if (g_down_since_ms == 0)
    g_down_since_ms = now_nonzero();
  const uint32_t down_ms = millis() - g_down_since_ms;
  if (down_ms >= kNudgeMs && millis() - g_last_nudge_ms >= kNudgeMs) {
    g_last_nudge_ms = millis();
    eventlog::log("wifi", "down %lus; reconnect nudge", (unsigned long)(down_ms / 1000));
    WiFi.reconnect();
  }
  if (down_ms >= kRebootMs) {
    // Deliberately also fires when the very first join never succeeds: if the
    // router was down at boot, a fresh boot every 15 min is a cheap retry.
    eventlog::log("wifi", "down %lu min; rebooting to self-heal", (unsigned long)(down_ms / 60000));
    eventlog::flush_tick();  // best effort: land the reason on SD first
    Serial.flush();
    esp_restart();
  }
}

bool connected() { return WiFi.status() == WL_CONNECTED; }
uint32_t drops() { return g_drops; }
int rssi() { return WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0; }

}  // namespace wifi_link
