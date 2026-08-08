// See mqtt_link.h. Connection, subscription and message handling verbatim
// from the pre-split firmware.
#include "net/mqtt_link.h"

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include <cmath>
#include <cstring>

#include "config.h"
#include "fan/control.h"
#include "sensors/climate.h"
#include "system/ota_rollback.h"

namespace mqtt_link {
namespace {

WiFiClient g_net;
PubSubClient g_mqtt(g_net);
uint32_t g_last_reconnect_ms = 0;
uint32_t g_up_ms = 0;
bool g_ever = false;

void on_message(char* topic, uint8_t* payload, unsigned int len) {
  char buf[16];
  if (len >= sizeof(buf))
    return;
  memcpy(buf, payload, len);
  buf[len] = '\0';
  if (strstr(topic, "/ts") != nullptr &&
      strncmp(topic, MQTT_SUB_BASE, strlen(MQTT_SUB_BASE)) == 0) {
    const long e = strtol(buf, nullptr, 10);
    if (e > 1700000000)
      climate::set_outdoor_epoch(e);
    return;
  }
  if (strcmp(topic, kTopicOutdoor) == 0) {
    char* end = nullptr;
    const float f = strtof(buf, &end);
    if (end != buf && isfinite(f) && f > -60 && f < 150)
      climate::set_outside_f(f);
    return;
  }
  if (strcmp(topic, kTopicSet) != 0 || len == 0 || len > 2)
    return;
  char* end = nullptr;
  long v = strtol(buf, &end, 10);
  if (end == buf || *end != '\0')
    return;
  // Manual only when it arrives OUTSIDE the retained-replay grace window:
  // a retained command replayed at connect is the broker remembering, not
  // the human acting.
  fan::apply(static_cast<int>(v), "mqtt", millis() - g_up_ms > kMqttGraceMs);
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
  char buf[4];
  snprintf(buf, sizeof(buf), "%d", speed);
  g_mqtt.publish(kTopicSet, buf, true);
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
  g_last_reconnect_ms = millis();
  char id[24];
  snprintf(id, sizeof(id), "garage-fan-%06llx", ESP.getEfuseMac() & 0xffffff);
  if (g_mqtt.connect(id, MQTT_USER, MQTT_PASS, kTopicAvail, 0, true, "offline")) {
    Serial.println("mqtt connected");
    g_up_ms = millis();
    g_ever = true;
    ota_rollback_mark_healthy();
    g_mqtt.publish(kTopicAvail, "online", true);
    publish_state(fan::speed());
    g_mqtt.subscribe(kTopicSet);
    g_mqtt.subscribe(kTopicOutdoor);
    g_mqtt.subscribe(MQTT_SUB_BASE "/ts");
  } else {
    Serial.printf("mqtt connect failed rc=%d\n", g_mqtt.state());
  }
}

}  // namespace

void init() {
  g_mqtt.setServer(MQTT_HOST, MQTT_PORT);
  g_mqtt.setCallback(on_message);
}

void tick() {
  ensure_connected();
  g_mqtt.loop();
}

bool connected() { return g_mqtt.connected(); }
bool ever_connected() { return g_ever; }

}  // namespace mqtt_link
