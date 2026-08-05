// Garage fan controller: the ESP32 replaces the iLiving wall controller,
// replaying its measured PWM protocol (docs/fan_protocol/PROTOCOL.md) through
// a BSS138 shifter. Control paths, all equivalent:
//   web UI    http://garage-fan.local/           (self-hosted, no broker)
//   HTTP API  GET /api/state, GET /api/set?speed=0..12
//   MQTT      garage/fan/set <- "0".."12"; state/availability published back
//   OTA       POST /update?token=... with the new firmware.bin
//
// OTA safety: images land in the inactive A/B slot (Update.h), and
// ota_rollback.{h,cpp} confirms an image only after it reaches the broker.
// An unconfirmed image that stays broker-less self-restarts after
// kUnconfirmedDeadlineMs; three such boots and ota_rollback flips back to the
// last confirmed slot. A confirmed image never self-restarts: the fan keeps
// running through network outages, and the RMT loop keeps transmitting on its
// own even while flash writes stall the CPU during an OTA upload.
#include <Arduino.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "esp_ota_ops.h"
#include "generated_config.h"
#include "ota_rollback.h"

#ifndef FAN_PWM_PIN
#define FAN_PWM_PIN 18  // A0 -> shifter LV1 -> HV1 -> fan D+
#endif
#ifndef FAN_SPARE_PIN
#define FAN_SPARE_PIN 17  // A1 -> shifter LV2; idles HIGH like the real D-
#endif
#ifndef FAN_HOSTNAME
#define FAN_HOSTNAME "garage-fan"
#endif
#ifndef FAN_OTA_TOKEN
#define FAN_OTA_TOKEN "iliving-ota"
#endif

static const char* kFwVersion = "1.1.1";

static constexpr uint16_t kPeriodUs = 9934;
// HIGH width per setting 0..12, measured from the wall controller.
static constexpr uint16_t kHighUs[13] = {kPeriodUs, 6457, 5862, 5066, 4868,
                                         4273,      3775, 3180, 2683, 2087,
                                         1590,      994,  497};

static const char* kTopicSet = "garage/fan/set";
static const char* kTopicState = "garage/fan/state";
static const char* kTopicAvail = "garage/fan/availability";
// An unconfirmed (fresh-OTA) image that has not reached the broker by this
// deadline restarts so ota_rollback's boot streak can count toward the flip.
static constexpr uint32_t kUnconfirmedDeadlineMs = 5 * 60 * 1000;

static WiFiClient g_net;
static PubSubClient g_mqtt(g_net);
static WebServer g_http(80);
static bool g_rmt_ready = false;
static bool g_services_up = false;
static bool g_ever_healthy = false;
static bool g_ota_authorized = false;
static rmt_data_t g_wave;
static int g_speed = 0;
static uint32_t g_last_reconnect_ms = 0;

static const char kPage[] PROGMEM = R"html(<!doctype html>
<html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Garage fan</title><style>
body{font-family:system-ui;background:#111;color:#eee;text-align:center;margin:0;padding:24px}
h1{font-size:1.2rem;font-weight:500}
#speed{font-size:4rem;margin:12px 0;font-variant-numeric:tabular-nums}
#grid{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;max-width:340px;margin:0 auto}
button{font-size:1.3rem;padding:14px 0;border:0;border-radius:10px;background:#333;color:#eee}
button.on{background:#2b7de9;color:#fff}
#off{grid-column:span 4;background:#552;color:#fff}
#meta{color:#888;font-size:.8rem;margin-top:18px}
</style></head><body>
<h1>Garage fan</h1><div id="speed">-</div>
<div id="grid"><button id="off" onclick="go(0)">off</button></div>
<div id="meta">-</div>
<script>
const g=document.getElementById('grid');
for(let i=1;i<=12;i++){const b=document.createElement('button');b.textContent=i;b.id='b'+i;b.onclick=()=>go(i);g.appendChild(b);}
async function go(n){await fetch('/api/set?speed='+n);poll();}
async function poll(){try{const s=await(await fetch('/api/state')).json();
document.getElementById('speed').textContent=s.speed===0?'off':s.speed;
for(let i=1;i<=12;i++)document.getElementById('b'+i).className=i===s.speed?'on':'';
document.getElementById('off').className=s.speed===0?'on':'';
document.getElementById('meta').textContent='fw '+s.fw+' ('+s.slot+') · rssi '+s.rssi+' dBm · mqtt '+(s.mqtt?'up':'down')+' · up '+s.uptime_s+'s';
}catch(e){document.getElementById('meta').textContent='unreachable';}}
setInterval(poll,2000);poll();
</script></body></html>)html";

static void drive_speed(int speed) {
  const uint16_t high_us = kHighUs[speed];
  if (high_us >= kPeriodUs) {
    if (g_rmt_ready) {
      rmtDeinit(FAN_PWM_PIN);
      g_rmt_ready = false;
    }
    pinMode(FAN_PWM_PIN, OUTPUT);
    digitalWrite(FAN_PWM_PIN, HIGH);
    return;
  }
  if (!g_rmt_ready) {
    if (!rmtInit(FAN_PWM_PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 1000000)) {
      Serial.println("rmtInit FAILED");
      return;
    }
    g_rmt_ready = true;
  }
  g_wave.level0 = 1;
  g_wave.duration0 = high_us;
  g_wave.level1 = 0;
  g_wave.duration1 = kPeriodUs - high_us;
  rmtWriteLooping(FAN_PWM_PIN, &g_wave, 1);
}

static void publish_state() {
  if (!g_mqtt.connected()) return;
  char buf[4];
  snprintf(buf, sizeof(buf), "%d", g_speed);
  g_mqtt.publish(kTopicState, buf, true);
}

static void apply_speed(int v, const char* source) {
  if (v < 0 || v > 12 || v == g_speed) return;
  g_speed = v;
  drive_speed(v);
  publish_state();
  Serial.printf("speed -> %d (high %u us) via %s\n", v, kHighUs[v], source);
}

static void state_json(char* out, size_t cap) {
  const esp_partition_t* run = esp_ota_get_running_partition();
  snprintf(out, cap,
           "{\"speed\":%d,\"fw\":\"%s\",\"slot\":\"%s\",\"confirmed\":%s,"
           "\"unhealthy_boots\":%u,\"rssi\":%d,\"mqtt\":%s,\"uptime_s\":%lu,"
           "\"ip\":\"%s\"}",
           g_speed, kFwVersion, run ? run->label : "?",
           ota_rollback_image_confirmed() ? "true" : "false",
           ota_rollback_unhealthy_boots(), WiFi.RSSI(),
           g_mqtt.connected() ? "true" : "false", millis() / 1000UL,
           WiFi.localIP().toString().c_str());
}

static void handle_state() {
  char buf[256];
  state_json(buf, sizeof(buf));
  g_http.send(200, "application/json", buf);
}

static void handle_set() {
  if (!g_http.hasArg("speed")) {
    g_http.send(400, "application/json", "{\"error\":\"speed required\"}");
    return;
  }
  const int v = g_http.arg("speed").toInt();
  if (v < 0 || v > 12) {
    g_http.send(400, "application/json", "{\"error\":\"0-12 only\"}");
    return;
  }
  apply_speed(v, "http");
  handle_state();
}

static void handle_update_upload() {
  HTTPUpload& up = g_http.upload();
  if (up.status == UPLOAD_FILE_START) {
    g_ota_authorized = g_http.arg("token") == FAN_OTA_TOKEN;
    if (!g_ota_authorized) {
      Serial.println("[OTA] rejected: bad token");
      return;
    }
    Serial.printf("[OTA] receiving %s\n", up.filename.c_str());
    // UPDATE_SIZE_UNKNOWN targets the inactive A/B slot.
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE && g_ota_authorized) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize)
      Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END && g_ota_authorized) {
    if (Update.end(true))
      Serial.printf("[OTA] received %u bytes ok\n", up.totalSize);
    else
      Update.printError(Serial);
  }
}

static void handle_update_done() {
  if (!g_ota_authorized) {
    g_http.send(403, "application/json", "{\"error\":\"bad token\"}");
    return;
  }
  if (Update.hasError()) {
    g_http.send(500, "application/json", "{\"error\":\"update failed\"}");
    return;
  }
  g_http.send(200, "application/json",
              "{\"ok\":true,\"note\":\"rebooting into new slot\"}");
  delay(300);  // let the response flush
  esp_restart();
}

static void on_message(char* topic, uint8_t* payload, unsigned int len) {
  if (strcmp(topic, kTopicSet) != 0 || len == 0 || len > 2) return;
  char buf[3] = {0};
  memcpy(buf, payload, len);
  char* end = nullptr;
  long v = strtol(buf, &end, 10);
  if (end == buf || *end != '\0') return;
  apply_speed(static_cast<int>(v), "mqtt");
  publish_state();  // echo even when unchanged so pollers settle
}

static void ensure_mqtt() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (g_mqtt.connected() || millis() - g_last_reconnect_ms < 3000) return;
  g_last_reconnect_ms = millis();
  char id[24];
  snprintf(id, sizeof(id), "garage-fan-%06llx", ESP.getEfuseMac() & 0xffffff);
  if (g_mqtt.connect(id, MQTT_USER, MQTT_PASS, kTopicAvail, 0, true,
                     "offline")) {
    Serial.println("mqtt connected");
    g_ever_healthy = true;
    ota_rollback_mark_healthy();  // broker reached: this image is good
    g_mqtt.publish(kTopicAvail, "online", true);
    publish_state();
    g_mqtt.subscribe(kTopicSet);  // retained set resumes the last speed
  } else {
    Serial.printf("mqtt connect failed rc=%d\n", g_mqtt.state());
  }
}

void setup() {
  pinMode(FAN_PWM_PIN, OUTPUT);
  digitalWrite(FAN_PWM_PIN, HIGH);  // boot with the fan off
  pinMode(FAN_SPARE_PIN, OUTPUT);
  digitalWrite(FAN_SPARE_PIN, HIGH);
  Serial.begin(115200);
  ota_rollback_check_at_boot();  // may flip slots and restart; returns fast
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(FAN_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  g_mqtt.setServer(MQTT_HOST, MQTT_PORT);
  g_mqtt.setCallback(on_message);
  g_http.on("/", []() { g_http.send_P(200, "text/html", kPage); });
  g_http.on("/api/state", handle_state);
  g_http.on("/api/set", handle_set);
  g_http.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
  g_http.onNotFound(
      []() { g_http.send(404, "application/json", "{\"error\":\"404\"}"); });
  Serial.printf("garage fan controller %s: waiting for wifi\n", kFwVersion);
}

void loop() {
  static wl_status_t last_wifi = WL_NO_SHIELD;
  const wl_status_t now = WiFi.status();
  if (now != last_wifi) {
    last_wifi = now;
    if (now == WL_CONNECTED) {
      Serial.printf("wifi up: %s\n", WiFi.localIP().toString().c_str());
      if (!g_services_up) {
        g_services_up = true;
        MDNS.begin(FAN_HOSTNAME);
        MDNS.addService("http", "tcp", 80);
        g_http.begin();
        Serial.printf("web ui: http://%s.local/\n", FAN_HOSTNAME);
      }
    }
  }
  ensure_mqtt();
  g_mqtt.loop();
  g_http.handleClient();
  // Fresh-OTA image that never reached the broker: restart so the rollback
  // streak counts. Confirmed images ride out network trouble instead -- the
  // fan matters more than the telemetry.
  if (!g_ever_healthy && !ota_rollback_image_confirmed() &&
      millis() > kUnconfirmedDeadlineMs) {
    Serial.println("[OTA] unconfirmed image never reached broker; restarting");
    Serial.flush();
    esp_restart();
  }
  delay(2);
}
