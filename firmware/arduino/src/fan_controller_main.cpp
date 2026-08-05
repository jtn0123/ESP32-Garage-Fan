// Garage fan controller: the ESP32 replaces the iLiving wall controller,
// replaying its PWM protocol (docs/fan_protocol/PROTOCOL.md; polarity
// corrected against the real fan 2026-08-04) through a BSS138 shifter.
//
// Control:  web UI http://garage-fan.local/ · /api/set?speed=0..12 ·
//           MQTT garage/fan/set · /api/raw?high_pct= (calibration)
// Auto:     differential thermostat vs outdoors (fan_auto_logic.h, natively
//           tested). Outdoor temp arrives on MQTT_SUB_BASE "/temp_f" from the
//           existing home/outdoor feed. Hotter outside -> min speed; hotter
//           inside -> ramp toward the user's max. Manual set disables auto.
// Climate:  BME280 samples every 5 min -> 24 h RAM ring + CSV on the microSD
//           card (monthly files, epoch-stamped once SNTP syncs) -> web graph
//           at 24 h / 7 d / 30 d ranges.
// Persist:  speed + auto config in NVS (restored before WiFi), commands also
//           retained on the broker; whichever answers first wins the tie.
// OTA:      POST /update?token=...; A/B slots with ota_rollback confirm.
#include <Adafruit_BME280.h>
#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <SD.h>
#include <SPI.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>

#include <ctime>

#include "esp_ota_ops.h"
#include "fan_auto_logic.h"
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
// Shared SPI bus with the e-ink FeatherWing: park every select high before
// touching the SD card (see storage.cpp in the sensor firmware).
#ifndef SD_CS_PIN
#define SD_CS_PIN 5
#endif
#ifndef SRAM_CS_PIN
#define SRAM_CS_PIN 6
#endif
#ifndef EPD_CS_PIN
#define EPD_CS_PIN 9
#endif

static const char* kFwVersion = "1.5.1";

static constexpr uint16_t kPeriodUs = 9934;
// HIGH width per setting 0..12, mirrored from the wall-controller captures
// after live fan testing showed this rig tracks the HIGH fraction (see
// PROTOCOL.md). Off = solid LOW. /api/raw re-derives empirically if needed.
static constexpr uint16_t kHighUs[13] = {0,    3477, 4072, 4868, 5066,
                                         5661, 6159, 6754, 7251, 7847,
                                         8344, 8940, 9437};

static const char* kTopicSet = "garage/fan/set";
static const char* kTopicState = "garage/fan/state";
static const char* kTopicAvail = "garage/fan/availability";
static const char* kTopicClimate = "garage/climate";
static const char* kTopicOutdoor = MQTT_SUB_BASE "/temp_f";
static constexpr uint32_t kUnconfirmedDeadlineMs = 5 * 60 * 1000;
static constexpr uint32_t kSampleMs = 5 * 60 * 1000;  // ring + SD cadence
static constexpr uint32_t kAutoTickMs = 30 * 1000;
static constexpr uint32_t kOutdoorStaleMs = 30 * 60 * 1000;
static constexpr uint32_t kMqttGraceMs = 10 * 1000;  // retained-replay window
static constexpr uint16_t kRingLen = 288;            // 24 h at 5 min
static constexpr uint16_t kGraphMaxPts = 288;

static WiFiClient g_net;
static PubSubClient g_mqtt(g_net);
static WebServer g_http(80);
static Adafruit_BME280 g_bme;
static Preferences g_prefs;
static bool g_bme_ok = false;
static bool g_sd_ok = false;
static bool g_rmt_ready = false;
static bool g_services_up = false;
static bool g_ever_healthy = false;
static bool g_ota_authorized = false;
static bool g_auto_on = false;
static int g_auto_max = 9;
static rmt_data_t g_wave;
static int g_speed = 0;
static float g_inside_c = NAN;
static float g_outside_c = NAN;
static uint32_t g_outside_ms = 0;
static uint32_t g_last_reconnect_ms = 0;
static uint32_t g_mqtt_up_ms = 0;
static uint32_t g_last_sample_ms = 0;
static uint32_t g_last_auto_ms = 0;
static float g_ring_t[kRingLen], g_ring_h[kRingLen], g_ring_p[kRingLen];
static uint16_t g_ring_count = 0;

static const char kPage[] PROGMEM = R"html(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Garage fan</title><style>
:root{--bg:#0e1116;--card:#171b21;--bd:#232a33;--tx:#e6e9ed;--mut:#8b94a1;--ac:#3b82f6;--ok:#22a06b}
body{font-family:system-ui;background:var(--bg);color:var(--tx);margin:0;padding:20px 16px 28px;display:flex;justify-content:center}
#wrap{width:100%;max-width:400px}
header{display:flex;align-items:center;justify-content:space-between;margin-bottom:6px}
h1{font-size:1.05rem;font-weight:600;margin:0;letter-spacing:.02em}
#dot{width:9px;height:9px;border-radius:50%;background:#555}
#dot.up{background:var(--ok)}
#speedcard{background:var(--card);border:1px solid var(--bd);border-radius:14px;padding:18px 16px 14px;margin-top:10px}
#speed{font-size:3.4rem;line-height:1;font-variant-numeric:tabular-nums;font-weight:600}
#of{color:var(--mut);font-size:.8rem;margin-top:2px}
#bar{height:6px;border-radius:3px;background:#232a33;margin:12px 0 16px;overflow:hidden}
#fill{height:100%;width:0%;background:var(--ac);border-radius:3px;transition:width .4s}
#grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}
button{font-size:1rem;padding:11px 0;border:1px solid var(--bd);border-radius:10px;background:#1d232b;color:var(--tx);cursor:pointer}
button.on{background:var(--ac);border-color:var(--ac);color:#fff}
#off{grid-column:span 4;background:#241a1a;border-color:#3a2626;color:#e0a9a9}
#off.on{background:#7f1d1d;border-color:#7f1d1d;color:#fff}
#autorow{display:flex;gap:10px;justify-content:space-between;align-items:center;margin-top:14px}
#autobtn{flex:1;display:flex;gap:8px;align-items:center;justify-content:center}
#autobtn.on{background:var(--ok);border-color:var(--ok);color:#fff}
select{font-size:.95rem;padding:9px 10px;border-radius:10px;background:#1d232b;color:var(--tx);border:1px solid var(--bd)}
#outinfo{color:var(--mut);font-size:.75rem;white-space:nowrap}
#climate{display:none;margin-top:14px}
#tiles{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}
.tile{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:10px 2px;text-align:center}
.tv{font-size:1.15rem;font-weight:600}.tl{font-size:.62rem;color:var(--mut);margin-top:3px;letter-spacing:.04em}
#ranges{display:flex;background:var(--card);border:1px solid var(--bd);border-radius:10px;padding:3px;margin-top:12px}
#ranges button{flex:1;font-size:.78rem;padding:7px 0;border:0;background:none;color:var(--mut)}
#ranges button.on{background:var(--ac);color:#fff;border-radius:8px}
canvas{width:100%;height:140px;margin-top:10px;background:var(--card);border:1px solid var(--bd);border-radius:12px}
#legend{font-size:.7rem;color:var(--mut);margin-top:6px;text-align:center}
.chip{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:4px;vertical-align:baseline}
footer{color:var(--mut);font-size:.7rem;margin-top:16px;text-align:center;line-height:1.7}
</style></head><body><div id="wrap">
<header><h1>Garage fan</h1><div id="dot"></div></header>
<div id="speedcard"><div id="speed">–</div><div id="of">of 12</div>
<div id="bar"><div id="fill"></div></div>
<div id="grid"><button id="off" onclick="go(0)">off</button></div>
<div id="autorow"><button id="autobtn" onclick="toggleAuto()">auto</button>
<select id="maxsel" onchange="setMax()"></select>
<span id="outinfo"></span></div></div>
<div id="climate"><div id="tiles">
<div class="tile"><div class="tv" id="tT">–</div><div class="tl">GARAGE °F</div></div>
<div class="tile"><div class="tv" id="tO">–</div><div class="tl">OUTSIDE °F</div></div>
<div class="tile"><div class="tv" id="tH">–</div><div class="tl">HUMIDITY %</div></div>
<div class="tile"><div class="tv" id="tP">–</div><div class="tl">PRESSURE</div></div>
</div><div id="ranges"><button id="r1" class="on" onclick="range(1)">24 h</button>
<button id="r7" onclick="range(7)">7 days</button>
<button id="r30" onclick="range(30)">30 days</button></div>
<canvas id="cv" width="736" height="280"></canvas>
<div id="legend"><span class="chip" style="background:#e8834a"></span>temp&ensp;
<span class="chip" style="background:#3b82f6"></span>humidity&ensp;
<span class="chip" style="background:#22a06b"></span>pressure</div></div>
<footer id="meta">–</footer>
</div><script>
let days=1,auto=false,maxs=9;
const g=document.getElementById('grid');
for(let i=1;i<=12;i++){const b=document.createElement('button');b.textContent=i;b.id='b'+i;b.onclick=()=>go(i);g.appendChild(b);}
const ms=document.getElementById('maxsel');
for(let i=1;i<=12;i++){const o=document.createElement('option');o.value=i;o.textContent='max '+i;ms.appendChild(o);}
async function go(n){await fetch('/api/set?speed='+n);poll();}
async function toggleAuto(){await fetch('/api/config?auto='+(auto?0:1));poll();}
async function setMax(){await fetch('/api/config?max='+ms.value);}
async function poll(){try{const s=await(await fetch('/api/state')).json();
auto=s.auto;maxs=s.auto_max;ms.value=maxs;
document.getElementById('speed').textContent=s.speed===0?'off':(s.speed<0?'raw':s.speed);
document.getElementById('fill').style.width=(s.speed>0?s.speed/12*100:0)+'%';
for(let i=1;i<=12;i++)document.getElementById('b'+i).className=i===s.speed?'on':'';
document.getElementById('off').className=s.speed===0?'on':'';
document.getElementById('dot').className=s.mqtt?'up':'';
document.getElementById('autobtn').className=auto?'on':'';
document.getElementById('autobtn').textContent=auto?'auto on':'auto off';
document.getElementById('outinfo').textContent=s.outside_f===null?'no outdoor feed':('outside '+s.outside_f.toFixed(1)+'°');
document.getElementById('tO').textContent=s.outside_f===null?'–':s.outside_f.toFixed(1);
let sd=s.sd_total_mb?('sd '+(s.sd_used_mb/1024).toFixed(2)+' / '+(s.sd_total_mb/1024).toFixed(1)+' GB'):'no sd card';
document.getElementById('meta').innerHTML='fw '+s.fw+' · '+s.slot+' · '+s.rssi+' dBm · '+sd+'<br>up '+Math.floor(s.uptime_s/3600)+'h '+Math.floor(s.uptime_s%3600/60)+'m';
}catch(e){document.getElementById('meta').textContent='unreachable';}}
function line(ctx,vals,color,W,H){if(vals.length<2)return;
let mn=Math.min(...vals),mx=Math.max(...vals);if(mx-mn<1e-6){mn-=1;mx+=1;}
const pad=10;ctx.strokeStyle=color;ctx.lineWidth=2;ctx.lineJoin='round';ctx.lineCap='round';ctx.beginPath();
let lx,ly;vals.forEach((v,i)=>{const x=pad+i*(W-2*pad)/(vals.length-1);
const y=H-pad-(v-mn)*(H-2*pad)/(mx-mn);i?ctx.lineTo(x,y):ctx.moveTo(x,y);lx=x;ly=y;});ctx.stroke();
ctx.fillStyle=color;ctx.beginPath();ctx.arc(lx,ly,3.5,0,7);ctx.fill();}
function range(d){days=d;[1,7,30].forEach(x=>document.getElementById('r'+x).className=x===d?'on':'');climate();}
async function climate(){try{
const c=await(await fetch('/api/sensors')).json();
document.getElementById('climate').style.display='block';
if(c.ok){
document.getElementById('tT').textContent=(c.temp_c*9/5+32).toFixed(1);
document.getElementById('tH').textContent=c.rh.toFixed(0);
document.getElementById('tP').textContent=(c.hpa*0.02953).toFixed(2);}
const h=await(await fetch('/api/history?days='+days)).json();
const cv=document.getElementById('cv'),ctx=cv.getContext('2d');
ctx.clearRect(0,0,cv.width,cv.height);
ctx.strokeStyle='#20262e';ctx.lineWidth=1;
for(let i=1;i<4;i++){ctx.beginPath();ctx.moveTo(10,i*cv.height/4);ctx.lineTo(cv.width-10,i*cv.height/4);ctx.stroke();}
if(!h.temp_c||h.temp_c.length<2){ctx.fillStyle='#5a6472';ctx.font='15px system-ui';
ctx.textAlign='center';ctx.fillText('waiting for data — one sample every 5 minutes',cv.width/2,cv.height/2);return;}
line(ctx,h.temp_c,'#e8834a',cv.width,cv.height);
line(ctx,h.rh,'#3b82f6',cv.width,cv.height);
line(ctx,h.hpa,'#22a06b',cv.width,cv.height);
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

// source: "boot", "http", "mqtt", "auto". Manual sources disable auto mode
// (the human explicitly grabbed the wheel); retained-replay right after the
// broker connects does not count as manual.
static void apply_speed(int v, const char* source) {
  if (v < 0 || v > 12 || v == g_speed) return;
  const bool manual = strcmp(source, "http") == 0 ||
                      (strcmp(source, "mqtt") == 0 &&
                       millis() - g_mqtt_up_ms > kMqttGraceMs);
  if (manual && g_auto_on) {
    g_auto_on = false;
    g_prefs.putBool("auto", false);
    Serial.println("auto mode off (manual override)");
  }
  g_speed = v;
  set_wave(kHighUs[v]);
  publish_state();
  if (strcmp(source, "mqtt") != 0 && g_mqtt.connected()) {
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", v);
    g_mqtt.publish(kTopicSet, buf, true);
  }
  g_prefs.putInt("speed", v);  // survives power loss even broker-less
  Serial.printf("speed -> %d via %s\n", v, source);
}

static float outside_c_fresh() {
  if (g_outside_ms == 0 || millis() - g_outside_ms > kOutdoorStaleMs)
    return NAN;
  return g_outside_c;
}

static void auto_tick() {
  if (!g_auto_on) return;
  if (g_bme_ok) {
    const float t = g_bme.readTemperature();
    if (!isnan(t)) g_inside_c = t;
  }
  FanAutoCfg cfg = kFanAutoDefaults;
  cfg.max_speed = g_auto_max;
  const int next = fan_auto_decide(g_inside_c, outside_c_fresh(),
                                   g_speed < 0 ? 0 : g_speed, cfg);
  if (next != g_speed) apply_speed(next, "auto");
}

static bool time_synced() { return time(nullptr) > 1700000000; }

static void sd_mount() {
  // Full teardown between attempts, per storage.cpp: a card interrupted
  // mid-transaction by a reset stops answering until the bus restarts from
  // silence. Called at boot and retried from loop() until a card appears.
  static const uint32_t kFreqs[] = {4000000, 1000000, 400000};
  for (size_t i = 0; i < 3; i++) {
    if (i > 0) {
      SD.end();
      SPI.end();
      delay(50);
      SPI.begin();
    }
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);
    pinMode(SRAM_CS_PIN, OUTPUT);
    digitalWrite(SRAM_CS_PIN, HIGH);
    pinMode(EPD_CS_PIN, OUTPUT);
    digitalWrite(EPD_CS_PIN, HIGH);
    if (SD.begin(SD_CS_PIN, SPI, kFreqs[i])) {
      g_sd_ok = true;
      Serial.printf("sd mounted at %lu Hz: %.1f/%.1f MB used\n",
                    (unsigned long)kFreqs[i], SD.usedBytes() / 1048576.0,
                    SD.totalBytes() / 1048576.0);
      return;
    }
  }
}

static void sd_log_sample(time_t now, float t, float h, float p) {
  if (!g_sd_ok) return;
  struct tm tm_now;
  gmtime_r(&now, &tm_now);
  char path[24];
  snprintf(path, sizeof(path), "/climate-%04d%02d.csv", tm_now.tm_year + 1900,
           tm_now.tm_mon + 1);
  File f = SD.open(path, FILE_APPEND);
  if (!f) {
    g_sd_ok = false;  // card yanked; re-detect on reboot
    return;
  }
  const float out = outside_c_fresh();
  char line[96];
  int n = snprintf(line, sizeof(line), "%ld,%.2f,%.1f,%.1f,%.2f,%d\n",
                   (long)now, t, h, p, isnan(out) ? -999.0f : out, g_speed);
  f.write((const uint8_t*)line, n);
  f.close();
}

static void sample_climate() {
  if (!g_bme_ok) {
    g_bme_ok = g_bme.begin(0x77, &Wire) || g_bme.begin(0x76, &Wire);
    if (!g_bme_ok) return;
    Serial.println("bme280 detected");
  }
  const float t = g_bme.readTemperature();
  const float h = g_bme.readHumidity();
  const float p = g_bme.readPressure() / 100.0f;
  if (isnan(t) || isnan(h) || isnan(p)) {
    g_bme_ok = false;
    return;
  }
  g_inside_c = t;
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
  if (time_synced()) sd_log_sample(time(nullptr), t, h, p);
  if (g_mqtt.connected()) {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"temp_c\":%.2f,\"rh\":%.1f,\"hpa\":%.1f}", t,
             h, p);
    g_mqtt.publish(kTopicClimate, buf, true);
  }
}

static void state_json(char* out, size_t cap) {
  const esp_partition_t* run = esp_ota_get_running_partition();
  const float oc = outside_c_fresh();
  char outside[16];
  if (isnan(oc))
    snprintf(outside, sizeof(outside), "null");
  else
    snprintf(outside, sizeof(outside), "%.1f", oc * 9 / 5 + 32);
  snprintf(out, cap,
           "{\"speed\":%d,\"auto\":%s,\"auto_max\":%d,\"outside_f\":%s,"
           "\"fw\":\"%s\",\"slot\":\"%s\",\"confirmed\":%s,"
           "\"unhealthy_boots\":%u,\"sensor\":%s,\"sd_total_mb\":%lu,"
           "\"sd_used_mb\":%lu,\"rssi\":%d,\"mqtt\":%s,\"uptime_s\":%lu,"
           "\"ip\":\"%s\"}",
           g_speed, g_auto_on ? "true" : "false", g_auto_max, outside,
           kFwVersion, run ? run->label : "?",
           ota_rollback_image_confirmed() ? "true" : "false",
           ota_rollback_unhealthy_boots(), g_bme_ok ? "true" : "false",
           g_sd_ok ? (unsigned long)(SD.totalBytes() / 1048576) : 0,
           g_sd_ok ? (unsigned long)(SD.usedBytes() / 1048576) : 0,
           WiFi.RSSI(), g_mqtt.connected() ? "true" : "false",
           millis() / 1000UL, WiFi.localIP().toString().c_str());
}

static void handle_state() {
  char buf[448];
  state_json(buf, sizeof(buf));
  g_http.send(200, "application/json", buf);
}

static void handle_config() {
  if (g_http.hasArg("auto")) {
    g_auto_on = g_http.arg("auto").toInt() != 0;
    g_prefs.putBool("auto", g_auto_on);
    Serial.printf("auto mode %s\n", g_auto_on ? "on" : "off");
  }
  if (g_http.hasArg("max")) {
    const int m = g_http.arg("max").toInt();
    if (m >= 1 && m <= 12) {
      g_auto_max = m;
      g_prefs.putInt("max", m);
    }
  }
  handle_state();
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

// Stream the month files covering [cutoff, now], decimating by stride into
// the caller's arrays. Two passes: count, then collect every (count/max)th.
static uint16_t sd_read_range(time_t cutoff, float* t, float* h, float* p,
                              uint16_t max_pts) {
  time_t now = time(nullptr);
  char paths[2][24];
  int npaths = 0;
  for (time_t at = cutoff; npaths < 2; at = now) {
    struct tm tmv;
    gmtime_r(&at, &tmv);
    char path[24];
    snprintf(path, sizeof(path), "/climate-%04d%02d.csv", tmv.tm_year + 1900,
             tmv.tm_mon + 1);
    if (npaths == 0 || strcmp(path, paths[0]) != 0)
      snprintf(paths[npaths++], 24, "%s", path);
    if (at == now) break;
  }
  uint32_t rows = 0;
  for (int pass = 0; pass < 2; pass++) {
    const uint32_t stride = pass ? (rows > max_pts ? rows / max_pts + 1 : 1) : 1;
    uint32_t seen = 0;
    uint16_t kept = 0;
    for (int i = 0; i < npaths; i++) {
      File f = SD.open(paths[i], FILE_READ);
      if (!f) continue;
      // Line-buffered scan; lines are short and epoch-prefixed.
      char line[96];
      size_t ll = 0;
      while (f.available()) {
        const char c = (char)f.read();
        if (c != '\n') {
          if (ll < sizeof(line) - 1) line[ll++] = c;
          continue;
        }
        line[ll] = '\0';
        ll = 0;
        const long epoch = strtol(line, nullptr, 10);
        if (epoch < cutoff) continue;
        if (pass == 0) {
          rows++;
          continue;
        }
        if (seen++ % stride) continue;
        if (kept >= max_pts) break;
        float tv, hv, pv;
        if (sscanf(line, "%*ld,%f,%f,%f", &tv, &hv, &pv) == 3) {
          t[kept] = tv;
          h[kept] = hv;
          p[kept] = pv;
          kept++;
        }
      }
      f.close();
    }
    if (pass == 1) return kept;
    if (rows == 0) return 0;
  }
  return 0;
}

static void handle_history() {
  const int days = g_http.hasArg("days") ? g_http.arg("days").toInt() : 1;
  String out;
  out.reserve(10000);
  if (days <= 1 || !g_sd_ok || !time_synced()) {
    out += "{\"interval_s\":300,";
    append_series(out, "temp_c", g_ring_t, g_ring_count, 1);
    out += ',';
    append_series(out, "rh", g_ring_h, g_ring_count, 0);
    out += ',';
    append_series(out, "hpa", g_ring_p, g_ring_count, 1);
    out += '}';
  } else {
    static float t[kGraphMaxPts], h[kGraphMaxPts], p[kGraphMaxPts];
    const time_t cutoff = time(nullptr) - (time_t)days * 86400;
    const uint16_t n = sd_read_range(cutoff, t, h, p, kGraphMaxPts);
    char head[48];
    snprintf(head, sizeof(head), "{\"interval_s\":%ld,",
             n > 1 ? (long)(days * 86400L / n) : 300L);
    out += head;
    append_series(out, "temp_c", t, n, 1);
    out += ',';
    append_series(out, "rh", h, n, 0);
    out += ',';
    append_series(out, "hpa", p, n, 1);
    out += '}';
  }
  g_http.send(200, "application/json", out);
}

// Calibration instrument: drive an arbitrary duty, no reflash per data point.
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
  set_wave((uint16_t)((uint32_t)kPeriodUs * pct / 100));
  g_speed = -1;
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"raw_high_pct\":%d}", pct);
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
  char buf[16];
  if (len >= sizeof(buf)) return;
  memcpy(buf, payload, len);
  buf[len] = '\0';
  if (strcmp(topic, kTopicOutdoor) == 0) {
    char* end = nullptr;
    const float f = strtof(buf, &end);
    if (end != buf && isfinite(f) && f > -60 && f < 150) {
      g_outside_c = (f - 32.0f) * 5.0f / 9.0f;
      g_outside_ms = millis();
    }
    return;
  }
  if (strcmp(topic, kTopicSet) != 0 || len == 0 || len > 2) return;
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
    g_mqtt_up_ms = millis();
    g_ever_healthy = true;
    ota_rollback_mark_healthy();
    g_mqtt.publish(kTopicAvail, "online", true);
    publish_state();
    g_mqtt.subscribe(kTopicSet);
    g_mqtt.subscribe(kTopicOutdoor);
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
  g_prefs.begin("fanctl", false);
  g_auto_on = g_prefs.getBool("auto", false);
  g_auto_max = g_prefs.getInt("max", 9);
  const int saved = g_prefs.getInt("speed", 0);
  if (saved > 0 && saved <= 12) {
    g_speed = saved;
    set_wave(kHighUs[saved]);  // resume before WiFi even exists
    Serial.printf("restored speed %d from nvs\n", saved);
  }
  Wire.begin();
  SPI.begin();
  sd_mount();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(FAN_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org");
  g_mqtt.setServer(MQTT_HOST, MQTT_PORT);
  g_mqtt.setCallback(on_message);
  g_http.on("/", []() { g_http.send_P(200, "text/html", kPage); });
  g_http.on("/api/state", handle_state);
  g_http.on("/api/set", handle_set);
  g_http.on("/api/raw", handle_raw);
  g_http.on("/api/config", handle_config);
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
  if (millis() - g_last_auto_ms >= kAutoTickMs) {
    g_last_auto_ms = millis();
    auto_tick();
    // Late card insertion mounts without a power cycle (quiet until it works).
    static uint8_t sd_backoff = 0;
    if (!g_sd_ok && ++sd_backoff >= 2) {
      sd_backoff = 0;
      sd_mount();
    }
  }
  if (!g_ever_healthy && !ota_rollback_image_confirmed() &&
      millis() > kUnconfirmedDeadlineMs) {
    Serial.println("[OTA] unconfirmed image never reached broker; restarting");
    Serial.flush();
    esp_restart();
  }
  delay(2);
}
