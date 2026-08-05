// Garage fan controller: the ESP32 replaces the iLiving wall controller,
// replaying its PWM protocol (docs/fan_protocol/PROTOCOL.md; polarity
// corrected against the real fan 2026-08-04) through a BSS138 shifter.
// Control paths, all equivalent:
//   web UI    http://garage-fan.local/           (self-hosted, no broker)
//   HTTP API  GET /api/state, /api/set?speed=0..12, /api/raw?high_pct=0..100
//   MQTT      garage/fan/set <- "0".."12"; state/availability published back
//   OTA       POST /update?token=... with the new firmware.bin
// Climate: a BME280 on the STEMMA/I2C port (optional, hot-detectable) samples
// every 5 min into a 24 h RAM ring, graphed on the page and published
// retained to garage/climate.
//
// OTA safety: images land in the inactive A/B slot; ota_rollback confirms an
// image only after it reaches the broker, and an unconfirmed image rolls back
// after three broker-less boots. The RMT loop keeps transmitting through
// network loss and OTA flash writes.
#include <Adafruit_BME280.h>
#include <Arduino.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>

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

static const char* kFwVersion = "1.4.0";

static constexpr uint16_t kPeriodUs = 9934;
// HIGH width per setting 0..12. Live fan testing (2026-08-04) showed this
// rig's fan tracks the HIGH fraction -- inverse of the wall-controller
// captures in PROTOCOL.md -- so this table is the measured one mirrored
// (high = period - controller_high). Off = solid LOW on this rig.
// Use /api/raw?high_pct=N to re-derive empirically if behavior shifts.
static constexpr uint16_t kHighUs[13] = {0,    3477, 4072, 4868, 5066,
                                         5661, 6159, 6754, 7251, 7847,
                                         8344, 8940, 9437};

static const char* kTopicSet = "garage/fan/set";
static const char* kTopicState = "garage/fan/state";
static const char* kTopicAvail = "garage/fan/availability";
static const char* kTopicClimate = "garage/climate";
static constexpr uint32_t kUnconfirmedDeadlineMs = 5 * 60 * 1000;
// 24 h of climate at 5-minute samples.
static constexpr uint32_t kSampleMs = 5 * 60 * 1000;
static constexpr uint16_t kRingLen = 288;

static WiFiClient g_net;
static PubSubClient g_mqtt(g_net);
static WebServer g_http(80);
static Adafruit_BME280 g_bme;
static bool g_bme_ok = false;
static bool g_rmt_ready = false;
static bool g_services_up = false;
static bool g_ever_healthy = false;
static bool g_ota_authorized = false;
static rmt_data_t g_wave;
static int g_speed = 0;
static uint32_t g_last_reconnect_ms = 0;
static uint32_t g_last_sample_ms = 0;
static float g_ring_t[kRingLen], g_ring_h[kRingLen], g_ring_p[kRingLen];
static uint16_t g_ring_count = 0;  // grows to kRingLen then ring shifts

static const char kPage[] PROGMEM = R"html(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Garage fan</title><style>
body{font-family:system-ui;background:#111;color:#eee;text-align:center;margin:0;padding:24px}
h1{font-size:1.2rem;font-weight:500}
#speed{font-size:4rem;margin:12px 0;font-variant-numeric:tabular-nums}
#grid{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;max-width:340px;margin:0 auto}
button{font-size:1.3rem;padding:14px 0;border:0;border-radius:10px;background:#333;color:#eee}
button.on{background:#2b7de9;color:#fff}
#off{grid-column:span 4;background:#552;color:#fff}
#climate{display:none;max-width:360px;margin:22px auto 0}
#tiles{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
.tile{background:#1c1c1c;border-radius:10px;padding:10px 4px}
.tv{font-size:1.4rem}.tl{font-size:.7rem;color:#888}
canvas{width:100%;height:130px;margin-top:12px;background:#1c1c1c;border-radius:10px}
#legend{font-size:.7rem;color:#888;margin-top:4px}
#meta{color:#888;font-size:.8rem;margin-top:18px}
</style></head><body>
<h1>Garage fan</h1><div id="speed">-</div>
<div id="grid"><button id="off" onclick="go(0)">off</button></div>
<div id="climate"><div id="tiles">
<div class="tile"><div class="tv" id="tT">-</div><div class="tl">temp °F</div></div>
<div class="tile"><div class="tv" id="tH">-</div><div class="tl">humidity %</div></div>
<div class="tile"><div class="tv" id="tP">-</div><div class="tl">inHg</div></div>
</div><canvas id="cv" width="700" height="260"></canvas>
<div id="legend"><span style="color:#e8834a">temp</span> ·
<span style="color:#2b7de9">humidity</span> ·
<span style="color:#4ac28a">pressure</span> · 24 h</div></div>
<div id="meta">-</div>
<script>
const g=document.getElementById('grid');
for(let i=1;i<=12;i++){const b=document.createElement('button');b.textContent=i;b.id='b'+i;b.onclick=()=>go(i);g.appendChild(b);}
async function go(n){await fetch('/api/set?speed='+n);poll();}
async function poll(){try{const s=await(await fetch('/api/state')).json();
document.getElementById('speed').textContent=s.speed===0?'off':(s.speed<0?'raw':s.speed);
for(let i=1;i<=12;i++)document.getElementById('b'+i).className=i===s.speed?'on':'';
document.getElementById('off').className=s.speed===0?'on':'';
document.getElementById('meta').textContent='fw '+s.fw+' ('+s.slot+') · rssi '+s.rssi+' dBm · mqtt '+(s.mqtt?'up':'down')+' · up '+s.uptime_s+'s';
}catch(e){document.getElementById('meta').textContent='unreachable';}}
function line(ctx,vals,color,W,H){if(vals.length<2)return;
let mn=Math.min(...vals),mx=Math.max(...vals);if(mx-mn<1e-6){mn-=1;mx+=1;}
const pad=8;ctx.strokeStyle=color;ctx.lineWidth=2;ctx.beginPath();
vals.forEach((v,i)=>{const x=pad+i*(W-2*pad)/(vals.length-1);
const y=H-pad-(v-mn)*(H-2*pad)/(mx-mn);i?ctx.lineTo(x,y):ctx.moveTo(x,y);});ctx.stroke();}
async function climate(){try{
const c=await(await fetch('/api/sensors')).json();
if(!c.ok){document.getElementById('climate').style.display='none';return;}
document.getElementById('climate').style.display='block';
document.getElementById('tT').textContent=(c.temp_c*9/5+32).toFixed(1);
document.getElementById('tH').textContent=c.rh.toFixed(0);
document.getElementById('tP').textContent=(c.hpa*0.02953).toFixed(2);
const h=await(await fetch('/api/history')).json();
const cv=document.getElementById('cv'),ctx=cv.getContext('2d');
ctx.clearRect(0,0,cv.width,cv.height);
line(ctx,h.temp_c,'#e8834a',cv.width,cv.height);
line(ctx,h.rh,'#2b7de9',cv.width,cv.height);
line(ctx,h.hpa,'#4ac28a',cv.width,cv.height);
}catch(e){}}
setInterval(poll,2000);poll();
setInterval(climate,60000);climate();
</script></body></html>)html";

static void set_wave(uint16_t high_us) {
  if (!g_rmt_ready) {
    if (!rmtInit(FAN_PWM_PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 1000000)) {
      Serial.println("rmtInit FAILED");
      return;
    }
    g_rmt_ready = true;
  }
  // Pin stays owned by RMT forever; flat lines are waves too. Handing the pin
  // back to plain GPIO left it stuck at the RMT's last level.
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

static void publish_state() {
  if (!g_mqtt.connected()) return;
  char buf[4];
  snprintf(buf, sizeof(buf), "%d", g_speed);
  g_mqtt.publish(kTopicState, buf, true);
}

static void apply_speed(int v, const char* source) {
  if (v < 0 || v > 12 || v == g_speed) return;
  g_speed = v;
  set_wave(kHighUs[v]);
  publish_state();
  // Retain the command itself so resume-after-power-loss works no matter
  // which path set the speed (our own echo no-ops on v == g_speed).
  if (strcmp(source, "mqtt") != 0 && g_mqtt.connected()) {
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", v);
    g_mqtt.publish(kTopicSet, buf, true);
  }
  Serial.printf("speed -> %d (high %u us) via %s\n", v, kHighUs[v], source);
}

static void sample_climate() {
  if (!g_bme_ok) {
    g_bme_ok = g_bme.begin(0x77, &Wire) || g_bme.begin(0x76, &Wire);
    if (!g_bme_ok) return;  // no sensor attached (yet); retry next tick
    Serial.println("bme280 detected");
  }
  const float t = g_bme.readTemperature();
  const float h = g_bme.readHumidity();
  const float p = g_bme.readPressure() / 100.0f;
  if (isnan(t) || isnan(h) || isnan(p)) {
    g_bme_ok = false;  // sensor unplugged; re-probe next tick
    return;
  }
  if (g_ring_count == kRingLen) {
    memmove(g_ring_t, g_ring_t + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_ring_h, g_ring_h + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_ring_p, g_ring_p + 1, (kRingLen - 1) * sizeof(float));
    g_ring_count--;
  }
  g_ring_t[g_ring_count] = t;
  g_ring_h[g_ring_count] = h;
  g_ring_p[g_ring_count] = p;
  g_ring_count++;
  if (g_mqtt.connected()) {
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"temp_c\":%.2f,\"rh\":%.1f,\"hpa\":%.1f}", t, h, p);
    g_mqtt.publish(kTopicClimate, buf, true);
  }
}

static void state_json(char* out, size_t cap) {
  const esp_partition_t* run = esp_ota_get_running_partition();
  snprintf(out, cap,
           "{\"speed\":%d,\"fw\":\"%s\",\"slot\":\"%s\",\"confirmed\":%s,"
           "\"unhealthy_boots\":%u,\"sensor\":%s,\"rssi\":%d,\"mqtt\":%s,"
           "\"uptime_s\":%lu,\"ip\":\"%s\"}",
           g_speed, kFwVersion, run ? run->label : "?",
           ota_rollback_image_confirmed() ? "true" : "false",
           ota_rollback_unhealthy_boots(), g_bme_ok ? "true" : "false",
           WiFi.RSSI(), g_mqtt.connected() ? "true" : "false",
           millis() / 1000UL, WiFi.localIP().toString().c_str());
}

static void handle_state() {
  char buf[288];
  state_json(buf, sizeof(buf));
  g_http.send(200, "application/json", buf);
}

static void handle_sensors() {
  if (!g_bme_ok || g_ring_count == 0) {
    g_http.send(200, "application/json", "{\"ok\":false}");
    return;
  }
  char buf[128];
  const uint16_t i = g_ring_count - 1;
  snprintf(buf, sizeof(buf),
           "{\"ok\":true,\"temp_c\":%.2f,\"rh\":%.1f,\"hpa\":%.1f}",
           g_ring_t[i], g_ring_h[i], g_ring_p[i]);
  g_http.send(200, "application/json", buf);
}

static void append_series(String& out, const char* name, const float* v,
                          uint16_t n, uint8_t decimals) {
  out += '"';
  out += name;
  out += "\":[";
  char num[16];
  for (uint16_t i = 0; i < n; i++) {
    dtostrf(v[i], 0, decimals, num);
    out += num;
    if (i + 1 < n) out += ',';
  }
  out += ']';
}

static void handle_history() {
  String out;
  out.reserve(9500);
  out += "{\"interval_s\":300,";
  append_series(out, "temp_c", g_ring_t, g_ring_count, 1);
  out += ',';
  append_series(out, "rh", g_ring_h, g_ring_count, 0);
  out += ',';
  append_series(out, "hpa", g_ring_p, g_ring_count, 1);
  out += '}';
  g_http.send(200, "application/json", out);
}

// Test instrument: drive an arbitrary duty so duty->airflow can be mapped
// empirically on the real fan, no reflash per data point.
static void handle_raw() {
  if (!g_http.hasArg("high_pct")) {
    g_http.send(400, "application/json", "{\"error\":\"high_pct 0-100\"}");
    return;
  }
  const int pct = g_http.arg("high_pct").toInt();
  if (pct < 0 || pct > 100) {
    g_http.send(400, "application/json", "{\"error\":\"high_pct 0-100\"}");
    return;
  }
  const uint16_t high_us = (uint16_t)((uint32_t)kPeriodUs * pct / 100);
  set_wave(high_us);
  g_speed = -1;  // raw mode; next /api/set re-enters the table
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"raw_high_pct\":%d,\"high_us\":%u}", pct,
           high_us);
  Serial.printf("raw duty: %d%% high (%u us)\n", pct, high_us);
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
  if (g_speed == -1 && v == 0) g_speed = -2;  // force off to re-apply after raw
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
  delay(300);
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
  publish_state();
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
    ota_rollback_mark_healthy();
    g_mqtt.publish(kTopicAvail, "online", true);
    publish_state();
    g_mqtt.subscribe(kTopicSet);
  } else {
    Serial.printf("mqtt connect failed rc=%d\n", g_mqtt.state());
  }
}

void setup() {
  pinMode(FAN_PWM_PIN, OUTPUT);
  digitalWrite(FAN_PWM_PIN, LOW);  // off on this rig = line low
  pinMode(FAN_SPARE_PIN, OUTPUT);
  digitalWrite(FAN_SPARE_PIN, HIGH);
  Serial.begin(115200);
  ota_rollback_check_at_boot();
  Wire.begin();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(FAN_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  g_mqtt.setServer(MQTT_HOST, MQTT_PORT);
  g_mqtt.setCallback(on_message);
  g_http.on("/", []() { g_http.send_P(200, "text/html", kPage); });
  g_http.on("/api/state", handle_state);
  g_http.on("/api/set", handle_set);
  g_http.on("/api/raw", handle_raw);
  g_http.on("/api/sensors", handle_sensors);
  g_http.on("/api/history", handle_history);
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
  if (g_last_sample_ms == 0 || millis() - g_last_sample_ms >= kSampleMs) {
    g_last_sample_ms = millis();
    sample_climate();
  }
  if (!g_ever_healthy && !ota_rollback_image_confirmed() &&
      millis() > kUnconfirmedDeadlineMs) {
    Serial.println("[OTA] unconfirmed image never reached broker; restarting");
    Serial.flush();
    esp_restart();
  }
  delay(2);
}
