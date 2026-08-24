// See mqtt_link.h. Connection, subscription and message handling verbatim
// from the pre-split firmware.
#include "net/mqtt_link.h"

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "net/creds.h"
#include "fan/control.h"
#include "net/plug.h"
#include "system/eventlog.h"
#include "system/ota_rollback.h"

namespace mqtt_link {
namespace {

WiFiClient g_net;
PubSubClient g_mqtt(g_net);
uint32_t g_last_reconnect_ms = 0;
uint32_t g_up_ms = 0;
uint32_t g_last_seen_up_ms = 0;  // last tick that observed a live session
int g_echoed_speed = -1;         // our own echo, awaiting its round trip
bool g_ever = false;

void on_message(char* topic, uint8_t* payload, unsigned int len) {
  char buf[16];
  if (len >= sizeof(buf))
    return;
  memcpy(buf, payload, len);
  buf[len] = '\0';
  // The outdoor temperature used to arrive here too, on the subscribed
  // home/outdoor "/temp_f" with a "/ts" epoch beside it. It was a relay of
  // a weather SERVICE, not a yard sensor, and it raced the firmware's own
  // open-meteo poll for the same variable -- see sensors/outdoor.h. Removed
  // in 1.23.0: the fan fetches its own weather and this link carries fan
  // commands only.
  if (strcmp(topic, kTopicSet) != 0 || len == 0 || len > 2)
    return;
  char* end = nullptr;
  long v = strtol(buf, &end, 10);
  if (end == buf || *end != '\0')
    return;
  // Manual only when it arrives OUTSIDE the retained-replay grace window (a
  // retained command replayed at connect is the broker remembering, not the
  // human acting) AND when it is not the echo of our own last publish coming
  // back around. apply() drops auto for a manual command even when the speed
  // is unchanged -- correct for a human, fatal for a feedback loop.
  const bool ours = (static_cast<int>(v) == g_echoed_speed);
  g_echoed_speed = -1;
  fan::apply(static_cast<int>(v), "mqtt", !ours && millis() - g_up_ms > kMqttGraceMs);
  publish_state(fan::speed());
}

}  // namespace

void publish_state(int speed) {
  if (!g_mqtt.connected())
    return;
  char buf[4];
  snprintf(buf, sizeof(buf), "%d", speed);
  g_mqtt.publish(kTopicState, buf, true);
}

void echo_set(int speed) {
  if (!g_mqtt.connected())
    return;
  // Remember what we are about to say so the round trip can be recognised.
  // We publish this retained so the broker's remembered command matches
  // reality -- but we also SUBSCRIBE to it, so the broker hands it straight
  // back, and on_message used to read our own echo as a human grabbing the
  // wheel and switch auto off. Auto could therefore make exactly one step
  // before disabling itself, forever; invisible until an outdoor reading
  // finally arrived and it tried to act (2026-08-09).
  g_echoed_speed = speed;
  char buf[4];
  snprintf(buf, sizeof(buf), "%d", speed);
  g_mqtt.publish(kTopicSet, buf, true);
}

void publish_alert(const char* payload) {
  if (!g_mqtt.connected())
    return;
  g_mqtt.publish(kTopicAlert, payload, true);
}

void publish_climate(float t, float h, float p) {
  if (!g_mqtt.connected())
    return;
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"temp_c\":%.2f,\"rh\":%.1f,\"hpa\":%.1f}", t, h, p);
  g_mqtt.publish(kTopicClimate, buf, true);
}

namespace {
static void ensure_connected() {
  if (WiFi.status() != WL_CONNECTED)
    return;
  if (g_mqtt.connected() || millis() - g_last_reconnect_ms < 3000)
    return;
  // Reaching here IS the evidence the session died -- connect() below only
  // runs because connected() is false -- so the reason is captured here and
  // reported on the "up" line. Edge detection alone proved unreliable: the
  // same drop was recorded by 1.14.10 and missed by 1.14.11, because whether
  // a tick observes the false depends on where the socket dies relative to
  // g_mqtt.loop(). A reconnect that cannot explain itself is what made this
  // fault invisible for so long (2026-08-09).
  const int why = g_mqtt.state();
  const uint32_t down_ms = millis() - g_last_seen_up_ms;
  g_last_reconnect_ms = millis();
  char id[24];
  snprintf(id, sizeof(id), "garage-fan-%06llx", ESP.getEfuseMac() & 0xffffff);
  // connect() is SYNCHRONOUS and can park the whole loop for as long as
  // PubSubClient's socket timeout (15 s by default) -- longer than the fan's
  // web server, the auto tick or anything else can tolerate. Timed because
  // that is the prime suspect for the ~16 s device-wide freeze measured from
  // the network on 1.14.8, which no sample or SD-flush timing accounted for.
  const uint32_t t_connect = millis();
  const bool ok =
      g_mqtt.connect(id, creds::mqtt_user(), creds::mqtt_pass(), kTopicAvail, 0, true, "offline");
  const uint32_t connect_ms = millis() - t_connect;
  if (ok) {
    eventlog::log("mqtt", "up (down %lums rc=%d connect=%lums)", (unsigned long)down_ms, why,
                  (unsigned long)connect_ms);
    g_up_ms = millis();
    g_ever = true;
    ota_rollback_mark_healthy();
    g_mqtt.publish(kTopicAvail, "online", true);
    publish_state(fan::speed());
    // Re-assert the standing alert (or the all-clear): the topic is retained,
    // and a reboot mid-alarm must not leave yesterday's alarm -- or worse,
    // yesterday's all-clear -- speaking for today's fan.
    {
      char alert[128];
      plug::alert_json(alert, sizeof(alert));
      g_mqtt.publish(kTopicAlert, alert, true);
    }
    g_mqtt.subscribe(kTopicSet);
  } else {
    // A FAILED attempt burns the socket timeout in full, so it is the more
    // expensive of the two paths and belongs on the tape, not just on a
    // serial port nobody is attached to.
    eventlog::log("mqtt", "connect failed rc=%d after %lums", g_mqtt.state(),
                  (unsigned long)connect_ms);
  }
}

// One observation of the link, logging the down edge wherever it is seen.
// state() at the moment of the edge still carries the failure that took the
// session (rc<0 transport, rc>0 broker), which is the whole diagnostic value.
void note_link_state() {
  static bool was_up = false;
  const bool up = g_mqtt.connected();
  if (up)
    g_last_seen_up_ms = millis();
  if (was_up && !up)
    eventlog::log("mqtt", "down rc=%d up_for=%lus", g_mqtt.state(),
                  (unsigned long)((millis() - g_up_ms) / 1000UL));
  was_up = up;
}

}  // namespace

void init() {
  // PubSubClient keeps the POINTER, not a copy; creds' buffers are static.
  g_mqtt.setServer(creds::mqtt_host(), creds::mqtt_port());
  g_mqtt.setCallback(on_message);
  // 60 s, not PubSubClient's 15 s default. The e-ink refresh blocks the loop
  // for ~17 s every 5 minutes, and no keepalive can be sent while it runs; the
  // broker (which allows 1.5x keepalive = 22.5 s) was dropping this client on
  // every single refresh and firing its will, so anything watching the fan saw
  // it flap all day. 60 s absorbs the refresh with room to spare and still
  // notices a genuinely dead link within 90 s.
  g_mqtt.setKeepAlive(60);
}

void tick() {
  // A drop surfaces at one of TWO moments, and a recorder that watches only
  // one of them logs an unexplained "up" later: either connected() is already
  // false on entry (the peer went away between ticks, and ensure_connected()
  // is about to heal it in this same tick), or g_mqtt.loop() itself notices
  // the keepalive lapse and tears the session down at the end of the tick.
  // 1.14.8 checked only after the reconnect and 1.14.9 only before it; both
  // recorded ups with no downs while the broker was firing this device's
  // will every five minutes (2026-08-09). So observe at both points.
  note_link_state();
  ensure_connected();
  g_mqtt.loop();
  note_link_state();
}

bool connected() { return g_mqtt.connected(); }
bool ever_connected() { return g_ever; }

}  // namespace mqtt_link
