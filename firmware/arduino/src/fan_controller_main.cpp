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
#include <Adafruit_EPD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_LC709203F.h>
#include <Adafruit_MAX1704X.h>
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
#include "lwip/sockets.h"
#include "esp_task_wdt.h"
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
#ifndef EPD_DC_PIN
#define EPD_DC_PIN 10
#endif

static const char* kFwVersion = "1.12.0";

static constexpr uint16_t kPeriodUs = 9934;
// HIGH width per setting 0..12, mirrored from the wall-controller captures
// after live fan testing showed this rig tracks the HIGH fraction (see
// PROTOCOL.md). Off = solid LOW. /api/raw re-derives empirically if needed.
static constexpr uint16_t kHighUs[13] = {0,    3477, 4072, 4868, 5066, 5661, 6159,
                                         6754, 7251, 7847, 8344, 8940, 9437};

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
static WiFiServer g_sse_srv(8081);
static WiFiClient g_sse[4];
// 2.13" mono SSD1680 FeatherWing, landscape; no busy/rst pins wired.
static Adafruit_SSD1680 g_epd(250, 122, EPD_DC_PIN, -1, EPD_CS_PIN, -1, -1);
static Adafruit_BME280 g_bme;
static Adafruit_MAX17048 g_max;
static Adafruit_LC709203F g_lc;
static Preferences g_prefs;
static bool g_bme_ok = false;
static uint8_t g_batt_kind = 0;  // 0 none, 1 MAX17048, 2 LC709203F, 3 raw LC
static float g_batt_v = NAN, g_batt_pct = NAN;
static float g_pct_hist[12];
static float g_v_hist[12];
static uint8_t g_pct_n = 0;  // 5-min cadence -> 1 h sliding window
static bool g_chg = false;   // sticky charging verdict, NVS-persisted
static bool g_sd_ok = false;
static uint8_t g_sd_fails = 0;  // give up retrying after 10; format resets
// Crash forensics for the SD path: the sentinel holds a magic value only
// while a mount is in flight. If a boot starts and finds it still set, the
// previous boot died inside the mount -- quarantine the card so it can never
// boot-loop the controller. RTC memory survives resets but not power loss.
RTC_DATA_ATTR static uint32_t rtc_sd_sentinel = 0;
// Breadcrumb of the op in flight when a boot dies, read back on next boot.
RTC_DATA_ATTR static char rtc_crumb[16] = {0};
static bool g_sd_quarantined = false;
static char g_last_death[48] = "none";
static char g_prev_death[48] = "none";  // the boot before this one, from NVS
static uint32_t g_boots = 0;            // lifetime boot count, NVS
static constexpr uint32_t kSdSentinelMagic = 0x5DDEAD01;

#define CRUMB(x) snprintf(rtc_crumb, sizeof(rtc_crumb), "%s", x)
#define CRUMB_CLEAR() (rtc_crumb[0] = 0)

static const char* reset_reason_str(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:
      return "poweron";
    case ESP_RST_SW:
      return "sw_reset";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "int_wdt";
    case ESP_RST_TASK_WDT:
      return "task_wdt";
    case ESP_RST_WDT:
      return "wdt";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_DEEPSLEEP:
      return "deepsleep";
    default:
      return "other";
  }
}
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
static time_t g_outdoor_epoch = 0;  // bridge's own timestamp topic, if any
// Board self-heating correction: charging current warms the board-mounted
// BME280 far more than idle operation does. User-tunable via
// /api/config?offc=&offi= (Celsius, added to the raw reading).
static float g_off_chg = -3.0f;
static float g_off_idle = -1.0f;
static uint32_t g_last_reconnect_ms = 0;
static uint32_t g_mqtt_up_ms = 0;
static uint32_t g_last_sample_ms = 0;
static uint32_t g_last_auto_ms = 0;
static float g_ring_t[kRingLen], g_ring_h[kRingLen], g_ring_p[kRingLen];
static float g_ring_o[kRingLen];   // outdoor F at sample time (NAN = none)
static int8_t g_ring_s[kRingLen];  // fan speed at sample time
static float g_ring_bv[kRingLen];  // battery volts at sample time (NAN = none)
static int8_t g_ring_c[kRingLen];  // charging verdict (1/0, -1 = no battery)
static time_t g_ring_end_ts = 0;
static uint16_t g_ring_count = 0;
// Runtime odometer + energy estimate (cubic fan law, ~105 W flat out).
static uint32_t g_run_total_s = 0, g_run_today_s = 0;
static uint32_t g_today_ymd = 0;
static float g_energy_wh = 0;
static uint32_t g_last_count_ms = 0;
static uint32_t g_last_nvs_ms = 0;
static bool g_epd_ok = false;
static uint32_t g_last_epd_ms = 0;
static int g_epd_speed_shown = -99;
static char g_token[40];
static int g_auto_min = 0;                           // rest speed once equalized (0 = off)
static float g_auto_onf = 2.5f, g_auto_offf = 1.5f;  // engage/release, deg F
static bool g_auto_high = false;                     // hysteresis latch for fan_auto_decide

static const char kPage[] PROGMEM = R"html(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#0e1116">
<link rel="manifest" href="/manifest.json">
<link rel="icon" href="/icon.svg">
<meta name="apple-mobile-web-app-capable" content="yes">
<title>Garage fan</title><style>
:root{--bg:#0e1116;--card:#171b21;--bd:#232a33;--tx:#e6e9ed;--mut:#8b94a1;--dim:#5a6472;--ac:#3b82f6;--ok:#22a06b;--or:#e8834a;--pu:#b98add}
*{box-sizing:border-box}
body{font-family:system-ui;background:var(--bg);color:var(--tx);margin:0;padding:0 18px 28px}
#wrap{max-width:1180px;margin:0 auto}
header{position:sticky;top:0;z-index:9;background:var(--bg);display:flex;align-items:center;gap:10px;padding:12px 0 10px;border-bottom:1px solid var(--bd);margin-bottom:14px}
#fanic{color:var(--ac);animation:spin 2s linear infinite;animation-play-state:paused}
@keyframes spin{to{transform:rotate(360deg)}}
h1{font-size:1.05rem;font-weight:600;margin:0}
#fwtag{color:var(--dim);font-size:.7rem}
#hdr{display:flex;gap:12px;align-items:center;margin-left:auto}
#hspeed{font-size:.8rem;color:var(--mut)}
#dot{width:9px;height:9px;border-radius:50%;background:#555}
#dot.up{background:var(--ok)}
#batt{display:flex;align-items:center;gap:5px;color:var(--mut);font-size:.72rem}
#bshell{width:22px;height:11px;border:1px solid var(--mut);border-radius:3px;padding:1px;position:relative}
#bshell:after{content:'';position:absolute;right:-4px;top:2px;width:2px;height:5px;background:var(--mut);border-radius:1px}
#bfill{height:100%;background:var(--ok);border-radius:1px;width:0%}
#gear{background:none;border:0;color:var(--mut);font-size:1.1rem;cursor:pointer;padding:2px 6px}
#main{display:grid;grid-template-columns:330px 1fr;gap:14px;align-items:start}
@media(max-width:880px){#main{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--bd);border-radius:14px;padding:16px}
#speed{font-size:3.2rem;line-height:1;font-variant-numeric:tabular-nums;font-weight:600}
#of{color:var(--mut);font-size:.8rem;margin-top:2px}
#bar{height:6px;border-radius:3px;background:#232a33;margin:12px 0 14px;overflow:hidden}
#fill{height:100%;width:0%;background:var(--ac);border-radius:3px;transition:width .4s}
#grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}
button{font-size:1rem;padding:10px 0;border:1px solid var(--bd);border-radius:10px;background:#1d232b;color:var(--tx);cursor:pointer}
button.on{background:var(--ac);border-color:var(--ac);color:#fff}
#off{grid-column:span 4;background:#241a1a;border-color:#3a2626;color:#e0a9a9}
#off.on{background:#7f1d1d;border-color:#7f1d1d;color:#fff}
#autorow{display:flex;gap:8px;align-items:center;margin-top:12px}
#autobtn{flex:1}
#autobtn.on{background:var(--ok);border-color:var(--ok);color:#fff}
select,input{font-size:.85rem;padding:8px 8px;border-radius:10px;background:#1d232b;color:var(--tx);border:1px solid var(--bd)}
#outinfo{color:var(--mut);font-size:.72rem;margin-top:8px}
#settings{display:none;margin-top:12px}
#settings h2{font-size:.85rem;font-weight:600;margin:0 0 10px;color:var(--mut)}
.srow{display:flex;justify-content:space-between;align-items:center;margin:8px 0;font-size:.85rem}
.srow input{width:90px;text-align:right}
#settings .sec{border-top:1px solid var(--bd);margin-top:12px;padding-top:10px}
.sbtn{width:100%;margin-top:10px;background:var(--ac);border-color:var(--ac);color:#fff}
#tiles{display:grid;grid-template-columns:repeat(4,1fr);gap:10px}
.tile{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:12px 10px 8px;text-align:center}
.tv{font-size:1.5rem;font-weight:600}.tl{font-size:.62rem;color:var(--mut);margin-top:3px;letter-spacing:.04em}
.src{font-size:.55rem;color:var(--dim);margin-top:2px}
.spark{width:100%;height:26px;margin-top:6px}
#stats{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:10px 14px;margin-top:10px;font-size:.78rem;color:var(--mut);display:none;line-height:1.9}
#stats b{color:var(--tx);font-weight:600}
#chartsec{grid-column:1/-1;display:none}
#crow{display:flex;align-items:center;margin:2px 0 8px}
#ranges{display:flex;background:var(--card);border:1px solid var(--bd);border-radius:10px;padding:3px}
#ranges button{font-size:.78rem;padding:6px 14px;border:0;background:none;color:var(--mut)}
#ranges button.on{background:var(--ac);color:#fff;border-radius:8px}
#xtime{margin-left:auto;font-size:.78rem;color:var(--mut);font-variant-numeric:tabular-nums}
.chart{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:8px 10px 4px;margin-bottom:8px}
.crow2{display:flex;align-items:baseline;gap:10px;font-size:.72rem;color:var(--mut)}
.crow2 .ct{font-weight:600;letter-spacing:.04em}
.crow2 .lg{color:var(--dim)}
.crow2 .ro{margin-left:auto;font-variant-numeric:tabular-nums;color:var(--tx)}
.chart canvas{display:block;width:100%}
#cv_t{height:200px}#cv_s{height:110px}#cv_h{height:96px}#cv_p{height:96px}#cv_b{height:110px}
#tax{height:24px;width:100%;display:block}
footer{color:var(--mut);font-size:.7rem;margin-top:14px;text-align:center;line-height:1.7}
</style></head><body><div id="wrap">
<header>
<svg id="fanic" viewBox="0 0 24 24" width="24" height="24" fill="currentColor"><circle cx="12" cy="12" r="2.4"/><g><path d="M12 10.2c-.4-3.6-2-6-4.6-5.6-2 .3-2.6 3.4-.8 5 1.3 1.2 3.2 1.4 5.4.6z"/><path d="M13.6 12.9c3.3 1.5 6.1 1.1 6.9-1.4.6-1.9-2-3.7-4.2-2.8-1.6.6-2.6 2.2-2.7 4.2z"/><path d="M10.4 13c-2.9 2.1-3.9 4.8-2.2 6.8 1.4 1.6 4.2.3 4.7-2 .3-1.7-.7-3.4-2.5-4.8z"/></g></svg>
<div><h1>Garage fan</h1><div id="fwtag">&ndash;</div></div>
<div id="hdr">
<span id="hspeed"></span>
<div id="batt" style="display:none"><span id="bpct"></span><div id="bshell"><div id="bfill"></div></div></div>
<div id="dot"></div><button id="gear" onclick="tog('settings')">&#9881;</button></div>
</header>
<div id="main">
<div>
<div class="card"><div id="speed">&ndash;</div><div id="of">of 12</div>
<div id="bar"><div id="fill"></div></div>
<div id="grid"><button id="off" onclick="go(0)">off</button></div>
<div id="autorow"><button id="autobtn" onclick="toggleAuto()">auto</button>
<select id="maxsel" onchange="setCfg('max',this.value)"></select>
<select id="minsel" onchange="setCfg('min',this.value)"></select></div>
<div id="outinfo"></div></div>
<div id="settings" class="card"><h2>auto mode</h2>
<div class="srow"><span>hold max above &deg;F</span><input id="s_onf" type="number" step="0.5" min="0.5"></div>
<div class="srow"><span>drop to low below &deg;F</span><input id="s_offf" type="number" step="0.5" min="0"></div>
<h2 class="sec">temperature offsets &deg;C</h2>
<div class="srow"><span>while charging</span><input id="s_offc" type="number" step="0.1"></div>
<div class="srow"><span>idle</span><input id="s_offi" type="number" step="0.1"></div>
<button class="sbtn" onclick="saveCfg()">save settings</button>
<h2 class="sec">firmware update (OTA)</h2>
<div class="srow"><input id="s_fw" type="file" style="width:100%"></div>
<div class="srow"><span>token</span><input id="s_tok" type="password"></div>
<button class="sbtn" style="background:#7f5a1d;border-color:#7f5a1d" onclick="doOta()">upload firmware</button>
<div class="srow" style="margin-top:10px"><a href="/download.csv" style="color:var(--ac)">download 24 h CSV</a></div></div>
</div>
<div>
<div id="tiles">
<div class="tile"><div class="tv" id="tO">&ndash;</div><div class="tl">OUTSIDE &deg;F</div><div class="src">yard feed</div><canvas class="spark" id="sp_o"></canvas></div>
<div class="tile"><div class="tv" id="tT">&ndash;</div><div class="tl">GARAGE &deg;F</div><div class="src">this board</div><canvas class="spark" id="sp_t"></canvas></div>
<div class="tile"><div class="tv" id="tH">&ndash;</div><div class="tl">HUMIDITY %</div><div class="src">this board</div><canvas class="spark" id="sp_h"></canvas></div>
<div class="tile"><div class="tv" id="tP">&ndash;</div><div class="tl">PRESSURE MB</div><div class="src">this board</div><canvas class="spark" id="sp_p"></canvas></div>
</div>
<div id="stats"></div>
</div>
<div id="chartsec">
<div id="crow"><div id="ranges"><button id="r1" class="on" onclick="range(1)">24 h</button>
<button id="r7" onclick="range(7)">7 days</button>
<button id="r30" onclick="range(30)">30 days</button></div>
<span id="xtime"></span></div>
<div class="chart"><div class="crow2"><span class="ct">TEMPERATURE &deg;F</span><span class="lg">garage solid &middot; outside dashed &middot; orange band = garage hotter</span><span class="ro" id="ro_t"></span></div><canvas id="cv_t"></canvas></div>
<div class="chart"><div class="crow2"><span class="ct">FAN SPEED</span><span class="lg">0&ndash;12</span><span class="ro" id="ro_s"></span></div><canvas id="cv_s"></canvas></div>
<div class="chart"><div class="crow2"><span class="ct">HUMIDITY %</span><span class="ro" id="ro_h"></span></div><canvas id="cv_h"></canvas></div>
<div class="chart"><div class="crow2"><span class="ct">PRESSURE MB</span><span class="ro" id="ro_p"></span></div><canvas id="cv_p"></canvas></div>
<div class="chart" id="ch_b" style="display:none"><div class="crow2"><span class="ct">BATTERY V</span><span class="lg">blue band = charging</span><span class="ro" id="ro_b"></span></div><canvas id="cv_b"></canvas></div>
<canvas id="tax"></canvas>
</div>
</div>
<footer id="meta">&ndash;</footer>
</div><script>
let days=1,auto=false,maxs=9,lastH=null,S=null,xi=-1;
const $=id=>document.getElementById(id);
const g=$('grid');
for(let i=1;i<=12;i++){const b=document.createElement('button');b.textContent=i;b.id='b'+i;b.onclick=()=>go(i);g.appendChild(b);}
const ms=$('maxsel');
for(let i=1;i<=12;i++){const o=document.createElement('option');o.value=i;o.textContent='max '+i;ms.appendChild(o);}
const ns=$('minsel');
for(let i=0;i<=12;i++){const o=document.createElement('option');o.value=i;o.textContent=i===0?'low off':'low '+i;ns.appendChild(o);}
function tog(id){const e=$(id);e.style.display=e.style.display==='block'?'none':'block';}
async function go(n){await fetch('/api/set?speed='+n);}
async function toggleAuto(){await fetch('/api/config?auto='+(auto?0:1));}
async function setCfg(k,v){await fetch('/api/config?'+k+'='+v);}
async function saveCfg(){await fetch('/api/config?onf='+$('s_onf').value+'&offf='+$('s_offf').value+'&offc='+$('s_offc').value+'&offi='+$('s_offi').value);tog('settings');}
async function doOta(){const f=$('s_fw').files[0];if(!f){alert('pick firmware.bin');return;}
const fd=new FormData();fd.append('firmware',f);
const r=await fetch('/update?token='+encodeURIComponent($('s_tok').value),{method:'POST',body:fd});
alert(await r.text());}
function render(s){
auto=s.auto;maxs=s.auto_max;ms.value=maxs;
if(s.auto_min!==undefined)ns.value=s.auto_min;
const sp=s.speed;
$('speed').textContent=sp===0?'off':(sp<0?'raw':sp);
$('hspeed').textContent=sp>0?('fan '+sp):'fan off';
$('fanic').style.animationPlayState=sp>0?'running':'paused';
if(sp>0)$('fanic').style.animationDuration=Math.max(.25,3.3-.25*sp)+'s';
$('fill').style.width=(sp>0?sp/12*100:0)+'%';
for(let i=1;i<=12;i++)$('b'+i).className=i===sp?'on':'';
$('off').className=sp===0?'on':'';
$('dot').className=s.mqtt?'up':'';
$('autobtn').className=auto?'on':'';
$('autobtn').textContent=auto?'auto on':'auto off';
$('fwtag').textContent='fw '+s.fw+' · '+s.slot;
$('outinfo').textContent=(s.outside_f===null?'no outdoor feed':('outside '+s.outside_f.toFixed(1)+'° right now'))+(auto?' · holds max above +'+s.on_f+'°F, low below +'+s.off_f+'°F':'');
$('tO').textContent=s.outside_f===null?'–':s.outside_f.toFixed(1);
if(s.batt){$('batt').style.display='flex';
$('bpct').textContent=(s.batt.chg?'⚡':'')+(s.batt.pct!==null?s.batt.pct+'%':s.batt.v.toFixed(2)+'V')+(s.batt.mvh!==null?' '+(s.batt.mvh>=0?'▲':'▼')+Math.abs(s.batt.mvh)+'mV/h':'');
const bp=s.batt.pct!==null?s.batt.pct:Math.max(0,Math.min(100,(s.batt.v-3.2)*100));
$('bfill').style.width=bp+'%';
$('bfill').style.background=s.batt.chg?'#3b82f6':(bp>40?'#22a06b':(bp>15?'#c9852a':'#c0392b'));}
if(!$('s_offc').value&&s.offc!==undefined){$('s_offc').value=s.offc;$('s_offi').value=s.offi;}
if(!$('s_onf').value&&s.on_f!==undefined){$('s_onf').value=s.on_f;$('s_offf').value=s.off_f;}
let sd=s.sd_total_mb?('sd '+(s.sd_used_mb/1024).toFixed(2)+'/'+(s.sd_total_mb/1024).toFixed(1)+'GB'):(s.sd_q?'sd quarantined':'no sd');
let bt=s.batt?(' · '+s.batt.v.toFixed(3)+'V'):'';
$('meta').innerHTML='fw '+s.fw+' · '+s.slot+' · '+s.rssi+'dBm · '+sd+bt+' · toff '+s.toff+' · link '+(s.mqtt?'up':'down')+' · up '+Math.floor(s.uptime_s/3600)+'h'+Math.floor(s.uptime_s%3600/60)+'m';
}
function connectSSE(){
try{const es=new EventSource('http://'+location.hostname+':8081/');
es.onmessage=e=>{try{render(JSON.parse(e.data))}catch(_){}};
}catch(_){}}
async function poll(){try{render(await(await fetch('/api/state')).json());}catch(e){$('meta').textContent='unreachable';}}
function mergeCache(h){
if(days!==1||!h.end_ts)return h;
let cache={};try{cache=JSON.parse(localStorage.gf24||'{}')}catch(_){}
const n=h.temp_c.length;
for(let i=0;i<n;i++){const ts=h.end_ts-(n-1-i)*h.interval_s;
cache[ts]={t:h.temp_c[i],r:h.rh[i],p:h.hpa[i],o:h.out_f?h.out_f[i]:null,s:h.spd?h.spd[i]:0,
b:h.batt_v?h.batt_v[i]:null,c:h.chg?h.chg[i]:-1};}
const cut=h.end_ts-86400;
const keys=Object.keys(cache).map(Number).filter(t=>t>=cut).sort((a,b)=>a-b);
const out={interval_s:h.interval_s,end_ts:keys[keys.length-1],temp_c:[],rh:[],hpa:[],out_f:[],spd:[],batt_v:[],chg:[]};
keys.forEach(t=>{const c=cache[t];out.temp_c.push(c.t);out.rh.push(c.r);out.hpa.push(c.p);out.out_f.push(c.o);out.spd.push(c.s);out.batt_v.push(c.b===undefined?null:c.b);out.chg.push(c.c===undefined?-1:c.c);});
const store={};keys.forEach(t=>store[t]=cache[t]);
try{localStorage.gf24=JSON.stringify(store)}catch(_){}
return out;}
// ---- chart engine ----
const L=46,R=12;
function build(h){
const n=h.temp_c?h.temp_c.length:0;
const ts=i=>h.end_ts?h.end_ts-(n-1-i)*h.interval_s:null;
const night=[];for(let i=0;i<n;i++){const t=ts(i);if(t===null){night.push(false);continue;}
const hr=new Date(t*1000).getHours();night.push(hr>=20||hr<6);}
return {n,ts,night,
tf:(h.temp_c||[]).map(v=>v==null?null:v*9/5+32),
of:(h.out_f||[]).map(v=>(v==null||v<=-100)?null:v),
rh:h.rh||[],hpa:h.hpa||[],spd:h.spd||[],
bv:(h.batt_v||[]).map(v=>v==null?null:v),
chg:h.chg||[]};}
function cvctx(cv){const w=cv.offsetWidth,hh=cv.offsetHeight,d=window.devicePixelRatio||1;
if(cv.width!==Math.round(w*d)||cv.height!==Math.round(hh*d)){cv.width=Math.round(w*d);cv.height=Math.round(hh*d);}
const c=cv.getContext('2d');c.setTransform(d,0,0,d,0,0);c.clearRect(0,0,w,hh);return{c,W:w,H:hh};}
function lim(vals){let mn=1e9,mx=-1e9;vals.forEach(v=>{if(v==null||isNaN(v))return;if(v<mn)mn=v;if(v>mx)mx=v;});
if(mn>mx)return null;if(mn===mx){mn-=1;mx+=1}const p=(mx-mn)*.12;return[mn-p,mx+p];}
function frame(c,W,H,st,yl,fmt){
const X=i=>L+i*(W-L-R)/Math.max(st.n-1,1);
if(days===1){c.fillStyle='rgba(255,255,255,.035)';let i=0;while(i<st.n){if(st.night[i]){let j=i;while(j<st.n&&st.night[j])j++;c.fillRect(X(i),0,X(j-1)-X(i)||1,H-2);i=j;}else i++;}}
c.strokeStyle='#20262e';c.lineWidth=1;c.fillStyle='#5a6472';c.font='10px system-ui';c.textAlign='right';
yl.forEach(v=>{const y=H-6-(v-yl.mn)*(H-16)/(yl.mx-yl.mn);
c.beginPath();c.moveTo(L,y);c.lineTo(W-R,y);c.stroke();c.fillText(fmt(v),L-5,y+3);});
return X;}
function ylv(mn,mx){const a=[mn,(mn+mx)/2,mx];a.mn=mn;a.mx=mx;return a;}
function plot(c,H,X,vals,mn,mx,col,dash,fillTo){
c.strokeStyle=col;c.lineWidth=1.8;c.lineJoin='round';if(dash)c.setLineDash([5,4]);
const Y=v=>H-6-(v-mn)*(H-16)/(mx-mn);
c.beginPath();let st=false;
vals.forEach((v,i)=>{if(v==null||isNaN(v)){st=false;return;}st?c.lineTo(X(i),Y(v)):c.moveTo(X(i),Y(v));st=true;});
c.stroke();c.setLineDash([]);return Y;}
function xhair(c,H,X,i){if(i<0)return;c.strokeStyle='rgba(230,233,237,.45)';c.lineWidth=1;
c.beginPath();c.moveTo(X(i),0);c.lineTo(X(i),H-2);c.stroke();}
function nodata(c,W,H,msg){c.fillStyle='#5a6472';c.font='13px system-ui';c.textAlign='center';c.fillText(msg,W/2,H/2);}
function drawTemp(){
const {c,W,H}=cvctx($('cv_t'));
if(S.n<2){nodata(c,W,H,'waiting for data — one sample every 5 minutes');return;}
const lm=lim(S.tf.concat(S.of.filter(v=>v!=null)));if(!lm){nodata(c,W,H,'no data');return;}
const yl=ylv(lm[0],lm[1]);const X=frame(c,W,H,S,yl,v=>v.toFixed(0)+'°');
const Y=v=>H-6-(v-yl.mn)*(H-16)/(yl.mx-yl.mn);
for(let i=0;i+1<S.n;i++){const a=S.tf[i],b=S.tf[i+1],oa=S.of[i],ob=S.of[i+1];
if(a==null||b==null||oa==null||ob==null)continue;
c.beginPath();c.moveTo(X(i),Y(a));c.lineTo(X(i+1),Y(b));c.lineTo(X(i+1),Y(ob));c.lineTo(X(i),Y(oa));c.closePath();
c.fillStyle=(a+b)/2>=(oa+ob)/2?'rgba(232,131,74,.16)':'rgba(59,130,246,.13)';c.fill();}
plot(c,H,X,S.of,yl.mn,yl.mx,'#8fa3b8',true);
plot(c,H,X,S.tf,yl.mn,yl.mx,'#e8834a',false);
let mni=-1,mxi=-1;S.tf.forEach((v,i)=>{if(v==null)return;if(mni<0||v<S.tf[mni])mni=i;if(mxi<0||v>S.tf[mxi])mxi=i;});
c.font='10px system-ui';
[[mxi,'#e8834a'],[mni,'#8fa3b8']].forEach(([i,col])=>{if(i<0)return;const v=S.tf[i];
c.fillStyle=col;c.beginPath();c.arc(X(i),Y(v),3,0,7);c.fill();
c.textAlign=X(i)>W-60?'right':'left';c.fillText(v.toFixed(1)+'°',X(i)+(X(i)>W-60?-6:6),Y(v)+(i===mxi?-4:10));});
xhair(c,H,X,xi);}
function drawSpeed(){
const {c,W,H}=cvctx($('cv_s'));
if(S.n<2||!S.spd.length){nodata(c,W,H,'no data');return;}
const yl=ylv(0,12);const X=frame(c,W,H,S,yl,v=>v.toFixed(0));
const Y=v=>H-6-v*(H-16)/12;
c.fillStyle='rgba(59,130,246,.35)';c.strokeStyle='#3b82f6';c.lineWidth=1.6;
c.beginPath();c.moveTo(X(0),Y(0));
for(let i=0;i<S.n;i++){const v=S.spd[i]||0;c.lineTo(X(i),Y(v));if(i+1<S.n)c.lineTo(X(i+1),Y(v));}
c.lineTo(X(S.n-1),Y(0));c.closePath();c.fill();
c.beginPath();let pv=S.spd[0]||0;c.moveTo(X(0),Y(pv));
for(let i=1;i<S.n;i++){const v=S.spd[i]||0;c.lineTo(X(i),Y(pv));c.lineTo(X(i),Y(v));pv=v;}
c.stroke();xhair(c,H,X,xi);}
function drawSimple(id,vals,col,fmt){
const {c,W,H}=cvctx($(id));
const vs=vals.filter(v=>v!=null&&!isNaN(v));
if(S.n<2||!vs.length){nodata(c,W,H,'no data');return;}
const lm=lim(vals);const yl=ylv(lm[0],lm[1]);
const X=frame(c,W,H,S,yl,fmt);
plot(c,H,X,vals,yl.mn,yl.mx,col,false);xhair(c,H,X,xi);}
function drawBatt(){
const has=S.bv.some(v=>v!=null);
$('ch_b').style.display=has?'block':'none';if(!has)return;
const {c,W,H}=cvctx($('cv_b'));
const lm=lim(S.bv);const yl=ylv(lm[0],lm[1]);
const X=i=>L+i*(W-L-R)/Math.max(S.n-1,1);
c.fillStyle='rgba(59,130,246,.14)';
let i=0;while(i<S.n){if(S.chg[i]===1){let j=i;while(j<S.n&&S.chg[j]===1)j++;c.fillRect(X(i),0,X(j-1)-X(i)||1,H-2);i=j;}else i++;}
frame(c,W,H,S,yl,v=>v.toFixed(2));
plot(c,H,X,S.bv,yl.mn,yl.mx,'#b98add',false);xhair(c,H,X,xi);}
function drawAxis(){
const {c,W,H}=cvctx($('tax'));
if(S.n<2||!lastH.end_ts)return;
c.fillStyle='#5a6472';c.font='11px system-ui';c.textAlign='center';
const X=i=>L+i*(W-L-R)/Math.max(S.n-1,1);
const step=Math.max(1,Math.round(S.n/7));
for(let i=0;i<S.n;i+=step){const t=S.ts(i);if(t===null)continue;const d=new Date(t*1000);
const lb=days===1?d.getHours().toString().padStart(2,'0')+':'+d.getMinutes().toString().padStart(2,'0'):(d.getMonth()+1)+'/'+d.getDate()+' '+d.getHours()+'h';
c.fillText(lb,Math.min(Math.max(X(i),24),W-28),15);}}
function drawAll(){if(!S)return;drawTemp();drawSpeed();
drawSimple('cv_h',S.rh,'#3b82f6',v=>v.toFixed(0));
drawSimple('cv_p',S.hpa,'#22a06b',v=>v.toFixed(1));
drawBatt();drawAxis();readout();}
function readout(){
if(!S||S.n<1)return;const i=xi>=0?xi:S.n-1;
const t=S.ts(i);
$('xtime').textContent=(xi>=0&&t)?new Date(t*1000).toLocaleString([],{month:'numeric',day:'numeric',hour:'2-digit',minute:'2-digit'}):'';
$('ro_t').textContent=(S.tf[i]!=null?S.tf[i].toFixed(1)+'°':'–')+(S.of[i]!=null?' / out '+S.of[i].toFixed(1)+'°':'');
$('ro_s').textContent=S.spd.length?(S.spd[i]>0?'fan '+S.spd[i]:'off'):'';
$('ro_h').textContent=S.rh[i]!=null?S.rh[i].toFixed(0)+'%':'';
$('ro_p').textContent=S.hpa[i]!=null?S.hpa[i].toFixed(1)+' mb':'';
$('ro_b').textContent=S.bv[i]!=null?S.bv[i].toFixed(2)+' V'+(S.chg[i]===1?' ⚡':''):'';}
function spark(id,vals,col){
const cv=$(id),{c,W,H}=cvctx(cv);
const lm=lim(vals);if(!lm||S.n<2)return;
c.strokeStyle=col;c.lineWidth=1.4;c.lineJoin='round';
const Y=v=>H-2-(v-lm[0])*(H-4)/(lm[1]-lm[0]);
c.beginPath();let st=false;
vals.forEach((v,i)=>{if(v==null||isNaN(v)){st=false;return;}
const x=i*W/(vals.length-1);st?c.lineTo(x,Y(v)):c.moveTo(x,Y(v));st=true;});
c.stroke();}
const cs=$('chartsec');
cs.addEventListener('mousemove',ev=>{
if(!S||S.n<2)return;const cv=$('cv_t'),r=cv.getBoundingClientRect();
const fx=(ev.clientX-r.left-L)/(r.width-L-R);
const i=Math.round(fx*(S.n-1));
xi=(i<0||i>=S.n||fx<-.02||fx>1.02)?-1:i;drawAll();});
cs.addEventListener('mouseleave',()=>{xi=-1;drawAll();});
window.addEventListener('resize',()=>drawAll());
function range(d){days=d;[1,7,30].forEach(x=>$('r'+x).className=x===d?'on':'');climate();}
async function climate(){try{
const c=await(await fetch('/api/sensors')).json();
$('chartsec').style.display='block';
if(c.ok){$('tT').textContent=(c.temp_c*9/5+32).toFixed(1);
$('tH').textContent=c.rh.toFixed(0);
$('tP').textContent=c.hpa.toFixed(1);}
let h=await(await fetch('/api/history?days='+days)).json();
h=mergeCache(h);lastH=h;S=build(h);xi=-1;drawAll();
spark('sp_o',S.of,'#8fa3b8');spark('sp_t',S.tf,'#e8834a');
spark('sp_h',S.rh,'#3b82f6');spark('sp_p',S.hpa,'#22a06b');
const st=await(await fetch('/api/stats')).json();
$('stats').style.display='block';
$('stats').innerHTML='24h: <b>'+st.t_min_f.toFixed(0)+'–'+st.t_max_f.toFixed(0)+'°F</b> avg <b>'+st.t_avg_f.toFixed(0)+'</b> · fan today <b>'+(st.run_today_s/3600).toFixed(1)+'h</b> · lifetime <b>'+(st.run_total_s/3600).toFixed(0)+'h</b> · <b>'+(st.energy_wh/1000).toFixed(2)+' kWh</b> est · now <b>'+st.watts_now.toFixed(0)+' W</b>';
}catch(e){}}
connectSSE();poll();setInterval(poll,15000);
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

static float fan_watts(int speed) {
  if (speed <= 0)
    return 0;
  const float f = speed / 12.0f;
  return 5.0f + 100.0f * f * f * f;  // cubic fan law, rough estimate
}

static void sse_push();

static void publish_state() {
  if (!g_mqtt.connected())
    return;
  char buf[4];
  snprintf(buf, sizeof(buf), "%d", g_speed);
  g_mqtt.publish(kTopicState, buf, true);
}

// source: "boot", "http", "mqtt", "auto". Manual sources disable auto mode
// (the human explicitly grabbed the wheel); retained-replay right after the
// broker connects does not count as manual.
static void apply_speed(int v, const char* source) {
  if (v < 0 || v > 12 || v == g_speed)
    return;
  const bool manual = strcmp(source, "http") == 0 ||
                      (strcmp(source, "mqtt") == 0 && millis() - g_mqtt_up_ms > kMqttGraceMs);
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
  sse_push();
  Serial.printf("speed -> %d via %s\n", v, source);
}

// Raw LC709203F access, bypassing the Adafruit library's begin() (whose IC
// version check fails on this board's chip even though it ACKs). CRC-8/ATM
// over the full I2C frame, per datasheet.
static uint8_t lc_crc(const uint8_t* d, size_t n) {
  uint8_t crc = 0;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
  }
  return crc;
}

static bool lc_write(uint8_t reg, uint16_t val) {
  uint8_t f[5] = {0x16, reg, (uint8_t)(val & 0xFF), (uint8_t)(val >> 8), 0};
  f[4] = lc_crc(f, 4);
  Wire.beginTransmission(0x0B);
  Wire.write(&f[1], 4);
  return Wire.endTransmission() == 0;
}

static bool lc_read(uint8_t reg, uint16_t* out) {
  Wire.beginTransmission(0x0B);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom((uint8_t)0x0B, (uint8_t)3) != 3)
    return false;
  const uint8_t lo = Wire.read(), hi = Wire.read(), crc = Wire.read();
  const uint8_t chk[5] = {0x16, reg, 0x17, lo, hi};
  if (lc_crc(chk, 5) != crc)
    return false;
  *out = ((uint16_t)hi << 8) | lo;
  return true;
}

// Boards in this project carry either fuel gauge depending on Feather rev --
// probe both libraries, then fall back to raw LC register access (memory:
// LC709203F lives at 0x0B, MAX17048 at 0x36; scan before assuming).
static void batt_begin() {
  if (g_batt_kind)
    return;
  // Raw LC access FIRST: this board's chip reports IC version 0x2AFF -- a
  // variant the Adafruit library rejects at begin() despite fully working
  // registers (proven 2026-08-05 via /api/battdebug: 3776 mV, 29% RSOC,
  // valid CRCs; requires repeated-start reads). The library path stays as
  // fallback for boards with recognized parts.
  uint16_t v = 0;
  if (lc_write(0x15, 0x0001) && lc_write(0x0B, 0x0055) &&  // 2x3200 = 6400 mAh
      lc_read(0x09, &v) && v > 2500 && v < 4600) {
    g_batt_kind = 3;
    Serial.printf("fuel gauge: LC709203F-variant via raw access (%u mV)\n", v);
    return;
  }
  if (g_max.begin(&Wire)) {
    g_batt_kind = 1;
    Serial.println("fuel gauge: MAX17048");
    return;
  }
  if (g_lc.begin(&Wire)) {
    g_lc.setPackAPA(0x56);  // dual-18650 6600 mAh pack, per the sensor node
    g_batt_kind = 2;
    Serial.println("fuel gauge: LC709203F");
  }
}

static void batt_read() {
  if (!g_batt_kind)
    return;
  float v = NAN, p = NAN;
  if (g_batt_kind == 3) {
    uint16_t mv = 0, soc = 0;
    if (lc_read(0x09, &mv))
      v = mv / 1000.0f;
    if (lc_read(0x0D, &soc) && soc <= 100)
      p = soc;
  } else {
    v = g_batt_kind == 1 ? g_max.cellVoltage() : g_lc.cellVoltage();
    p = g_batt_kind == 1 ? g_max.cellPercent() : g_lc.cellPercent();
  }
  if (v > 2.0f && v < 5.0f) {
    g_batt_v = v;
    if (!isnan(p))
      g_batt_pct = p > 100 ? 100 : p;
  }
}

// Sticky charging verdict, updated once per 5-min battery sample: enter on a
// clearly rising voltage slope, leave only on a clearly falling one. In the
// plateau between (trickle charge, jitter) the last verdict stands -- a
// flapping detector swings the temperature offset by ~12 C and poisons every
// consumer of garage temp (tiles, ring, auto mode). Persisted so a reboot
// mid-charge keeps the right offset.
static void chg_update() {
  if (g_pct_n < 3)
    return;  // not enough history to judge; hold
  const float hours = (g_pct_n - 1) * 5.0f / 60.0f;
  const float mvh = (g_batt_v - g_v_hist[0]) * 1000.0f / hours;
  const float dpct = g_pct_hist[0] - g_pct_hist[g_pct_n - 1];  // + = draining
  // +5 mV/h is unmistakably inbound power on a big pack; 4.17 V is float.
  bool next = g_chg;
  if (mvh >= 5.0f || g_batt_v >= 4.17f || dpct < -0.3f)
    next = true;
  else if (mvh <= -5.0f || dpct > 0.3f)
    next = false;
  if (next != g_chg) {
    g_chg = next;
    g_prefs.putBool("chg", g_chg);
    Serial.printf("charging: %s\n", g_chg ? "yes" : "no");
  }
}

// Discharge slope over the last hour of 5-min points -> hours remaining.
// Rising charge or a flat line means no meaningful ETA.
static void batt_eta(bool* charging, float* eta_h) {
  *charging = g_chg;
  *eta_h = NAN;
  if (g_pct_n < 6 || *charging)
    return;
  const float hours = (g_pct_n - 1) * 5.0f / 60.0f;
  const float delta = g_pct_hist[0] - g_pct_hist[g_pct_n - 1];  // + = draining
  if (delta > 0.2f)
    *eta_h = g_batt_pct / (delta / hours);
}

static bool time_synced();

static bool is_charging() { return g_chg; }

static float temp_corrected(float raw) { return raw + (is_charging() ? g_off_chg : g_off_idle); }

static float outside_c_fresh() {
  // A bridge that publishes its own epoch timestamp gives real freshness --
  // retained redelivery of an old snapshot reads as stale, as it should.
  // Feeds without a ts topic fall back to receipt-time freshness.
  if (g_outdoor_epoch > 0) {
    if (!time_synced() || time(nullptr) - g_outdoor_epoch > 1800)
      return NAN;
    return g_outside_c;
  }
  if (g_outside_ms == 0 || millis() - g_outside_ms > kOutdoorStaleMs)
    return NAN;
  return g_outside_c;
}

static void auto_tick() {
  if (!g_auto_on)
    return;
  if (g_bme_ok) {
    const float t = g_bme.readTemperature();
    if (!isnan(t))
      g_inside_c = temp_corrected(t);
  }
  FanAutoCfg cfg = kFanAutoDefaults;
  cfg.min_speed = g_auto_min;
  cfg.max_speed = g_auto_max;
  cfg.on_delta_c = g_auto_onf * 5 / 9;  // user thinks in F; logic runs in C
  cfg.off_delta_c = g_auto_offf * 5 / 9;
  const int next =
      fan_auto_decide(g_inside_c, outside_c_fresh(), g_speed < 0 ? 0 : g_speed, &g_auto_high, cfg);
  if (next != g_speed)
    apply_speed(next, "auto");
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
      Serial.printf("sd mounted at %lu Hz: %.1f/%.1f MB used\n", (unsigned long)kFreqs[i],
                    SD.usedBytes() / 1048576.0, SD.totalBytes() / 1048576.0);
      return;
    }
  }
}

static void sd_mount_guarded() {
  rtc_sd_sentinel = kSdSentinelMagic;
  CRUMB("sd_mount");
  sd_mount();
  CRUMB_CLEAR();
  rtc_sd_sentinel = 0;
}

static void sd_log_sample(time_t now, float t, float h, float p) {
  if (!g_sd_ok)
    return;
  struct tm tm_now;
  gmtime_r(&now, &tm_now);
  char path[24];
  snprintf(path, sizeof(path), "/climate-%04d%02d.csv", tm_now.tm_year + 1900, tm_now.tm_mon + 1);
  CRUMB("sd_write");
  File f = SD.open(path, FILE_APPEND);
  if (!f) {
    CRUMB_CLEAR();
    g_sd_ok = false;  // card yanked; re-detect on reboot
    return;
  }
  const float out = outside_c_fresh();
  char line[96];
  int n = snprintf(line, sizeof(line), "%ld,%.2f,%.1f,%.1f,%.2f,%d\n", (long)now, t, h, p,
                   isnan(out) ? -999.0f : out, g_speed);
  f.write((const uint8_t*)line, n);
  f.close();
  CRUMB_CLEAR();
}

static void sample_climate() {
  if (!g_bme_ok) {
    g_bme_ok = g_bme.begin(0x77, &Wire) || g_bme.begin(0x76, &Wire);
    if (!g_bme_ok)
      return;
    Serial.println("bme280 detected");
  }
  const float t_raw = g_bme.readTemperature();
  const float h = g_bme.readHumidity();
  const float p = g_bme.readPressure() / 100.0f;
  if (isnan(t_raw) || isnan(h) || isnan(p)) {
    g_bme_ok = false;
    return;
  }
  const float t = temp_corrected(t_raw);
  g_inside_c = t;
  // Battery first so this sample's ring row records the fresh reading and
  // the charging verdict it produced, not the 5-minute-old one.
  batt_begin();
  batt_read();
  if (!isnan(g_batt_v)) {
    if (g_pct_n == 12) {
      memmove(g_pct_hist, g_pct_hist + 1, 11 * sizeof(float));
      memmove(g_v_hist, g_v_hist + 1, 11 * sizeof(float));
      g_pct_n--;
    }
    g_pct_hist[g_pct_n] = isnan(g_batt_pct) ? 0 : g_batt_pct;
    g_v_hist[g_pct_n] = g_batt_v;
    g_pct_n++;
    chg_update();
  }
  if (g_ring_count == kRingLen) {
    memmove(g_ring_t, g_ring_t + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_ring_h, g_ring_h + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_ring_p, g_ring_p + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_ring_o, g_ring_o + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_ring_s, g_ring_s + 1, (kRingLen - 1) * sizeof(int8_t));
    memmove(g_ring_bv, g_ring_bv + 1, (kRingLen - 1) * sizeof(float));
    memmove(g_ring_c, g_ring_c + 1, (kRingLen - 1) * sizeof(int8_t));
    g_ring_count--;
  }
  g_ring_t[g_ring_count] = t;
  g_ring_h[g_ring_count] = h;
  g_ring_p[g_ring_count] = p;
  const float oc = outside_c_fresh();
  g_ring_o[g_ring_count] = isnan(oc) ? NAN : oc * 9 / 5 + 32;
  g_ring_s[g_ring_count] = (int8_t)(g_speed < 0 ? 0 : g_speed);
  g_ring_bv[g_ring_count] = g_batt_kind ? g_batt_v : NAN;
  g_ring_c[g_ring_count] = g_batt_kind ? (g_chg ? 1 : 0) : -1;
  g_ring_count++;
  if (time_synced())
    g_ring_end_ts = time(nullptr);
  if (time_synced())
    sd_log_sample(time(nullptr), t, h, p);
  if (g_mqtt.connected()) {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"temp_c\":%.2f,\"rh\":%.1f,\"hpa\":%.1f}", t, h, p);
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
  bool chg = false;
  float eta = NAN;
  batt_eta(&chg, &eta);
  char batt[96];
  if (g_batt_kind && !isnan(g_batt_v)) {
    char mvh[16] = "null";
    if (g_pct_n >= 3) {
      const float hours = (g_pct_n - 1) * 5.0f / 60.0f;
      snprintf(mvh, sizeof(mvh), "%.0f", (g_batt_v - g_v_hist[0]) * 1000.0f / hours);
    }
    char etas[16] = "null";
    if (!isnan(eta))
      snprintf(etas, sizeof(etas), "%.1f", eta);
    char pcts[16] = "null";
    if (!isnan(g_batt_pct))
      snprintf(pcts, sizeof(pcts), "%.0f", g_batt_pct);
    snprintf(batt, sizeof(batt), "{\"v\":%.3f,\"pct\":%s,\"chg\":%s,\"eta_h\":%s,\"mvh\":%s}",
             g_batt_v, pcts, chg ? "true" : "false", etas, mvh);
  } else {
    snprintf(batt, sizeof(batt), "null");
  }
  snprintf(out, cap,
           "{\"speed\":%d,\"auto\":%s,\"auto_max\":%d,\"auto_min\":%d,"
           "\"on_f\":%.1f,\"off_f\":%.1f,\"outside_f\":%s,"
           "\"toff\":%.1f,\"offc\":%.1f,\"offi\":%.1f,"
           "\"fw\":\"%s\",\"slot\":\"%s\",\"confirmed\":%s,"
           "\"unhealthy_boots\":%u,\"sensor\":%s,\"last_reset\":\"%s\","
           "\"boots\":%lu,\"prev_death\":\"%s\","
           "\"sd_q\":%s,"
           "\"sd_total_mb\":%lu,"
           "\"sd_used_mb\":%lu,\"batt\":%s,\"rssi\":%d,\"mqtt\":%s,"
           "\"uptime_s\":%lu,\"ip\":\"%s\"}",
           g_speed, g_auto_on ? "true" : "false", g_auto_max, g_auto_min, g_auto_onf, g_auto_offf,
           outside, is_charging() ? g_off_chg : g_off_idle, g_off_chg, g_off_idle, kFwVersion,
           run ? run->label : "?", ota_rollback_image_confirmed() ? "true" : "false",
           ota_rollback_unhealthy_boots(), g_bme_ok ? "true" : "false", g_last_death,
           (unsigned long)g_boots, g_prev_death, g_sd_quarantined ? "true" : "false",
           g_sd_ok ? (unsigned long)(SD.totalBytes() / 1048576) : 0,
           g_sd_ok ? (unsigned long)(SD.usedBytes() / 1048576) : 0, batt, WiFi.RSSI(),
           g_mqtt.connected() ? "true" : "false", millis() / 1000UL,
           WiFi.localIP().toString().c_str());
}

// Never block on an SSE peer. NetworkClient::write retries a 1 s select up
// to 10 times per call, so one sleeping laptop with a full socket buffer
// costs 10 s per print -- sse_push's three prints hit the 30 s task
// watchdog and panic the board (proven live 2026-08-05: crash loop after
// v1.11.0 with several dashboards open). Instead: zero-timeout select +
// MSG_DONTWAIT send, and any peer that can't take the whole frame right
// now is dropped -- the browser's EventSource auto-reconnects.
static void sse_send(WiFiClient& c, const char* buf, int n) {
  const int s = c.fd();
  if (s < 0) {
    c.stop();
    return;
  }
  fd_set set;
  timeval tv{0, 0};
  FD_ZERO(&set);
  FD_SET(s, &set);
  if (select(s + 1, nullptr, &set, nullptr, &tv) <= 0 || !FD_ISSET(s, &set)) {
    c.stop();
    return;
  }
  if (send(s, buf, n, MSG_DONTWAIT) != n)
    c.stop();
}

static void sse_push() {
  char st[768];
  char frame[832];
  state_json(st, sizeof(st));
  const int n = snprintf(frame, sizeof(frame), "data: %s\n\n", st);
  for (auto& c : g_sse) {
    if (c && c.connected())
      sse_send(c, frame, n);
  }
}

static void sse_accept() {
  WiFiClient nc = g_sse_srv.accept();
  if (!nc)
    return;
  for (auto& c : g_sse) {
    if (!c || !c.connected()) {
      c = nc;
      c.setNoDelay(true);
      char st[768];
      char frame[1024];
      state_json(st, sizeof(st));
      const int n = snprintf(frame, sizeof(frame),
                             "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                             "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n"
                             "Connection: keep-alive\r\n\r\nretry: 3000\n\ndata: %s\n\n",
                             st);
      sse_send(c, frame, n);
      return;
    }
  }
  nc.stop();  // table full
}

static void epd_render() {
  if (!g_epd_ok)
    return;
  CRUMB("epd");
  g_epd.clearBuffer();
  g_epd.fillScreen(EPD_WHITE);
  g_epd.setTextColor(EPD_BLACK);
  char b[32];
  const float tin = g_ring_count ? g_ring_t[g_ring_count - 1] : NAN;
  g_epd.setTextSize(4);
  g_epd.setCursor(4, 18);
  if (isnan(tin))
    g_epd.print("--.-");
  else
    g_epd.printf("%.1f", tin * 9 / 5 + 32);
  g_epd.setTextSize(1);
  g_epd.setCursor(6, 54);
  g_epd.print("GARAGE F");
  const float rh = g_ring_count ? g_ring_h[g_ring_count - 1] : NAN;
  g_epd.setCursor(70, 54);
  if (!isnan(rh))
    g_epd.printf("RH %.0f%%", rh);
  g_epd.setTextSize(3);
  g_epd.setCursor(158, 8);
  if (g_speed <= 0)
    g_epd.print("OFF");
  else
    g_epd.printf("F%d", g_speed);
  g_epd.setTextSize(1);
  g_epd.setCursor(158, 38);
  g_epd.print(g_auto_on ? "AUTO ON" : "AUTO OFF");
  const float oc = outside_c_fresh();
  g_epd.setCursor(158, 52);
  if (isnan(oc))
    g_epd.print("OUT --");
  else
    g_epd.printf("OUT %.1fF", oc * 9 / 5 + 32);
  g_epd.setCursor(158, 66);
  if (!isnan(g_batt_v))
    g_epd.printf("BAT %.2fV %.0f%%", g_batt_v, isnan(g_batt_pct) ? 0 : g_batt_pct);
  g_epd.setCursor(6, 76);
  g_epd.printf("run today %luh%02lum  total %luh", (unsigned long)(g_run_today_s / 3600),
               (unsigned long)(g_run_today_s % 3600 / 60), (unsigned long)(g_run_total_s / 3600));
  g_epd.setCursor(6, 106);
  g_epd.printf("%s  fw %s", WiFi.localIP().toString().c_str(), kFwVersion);
  g_epd.display();
  CRUMB_CLEAR();
  g_last_epd_ms = millis();
  g_epd_speed_shown = g_speed;
}

static void handle_stats() {
  float tmin = NAN, tmax = NAN, tsum = 0;
  for (uint16_t i = 0; i < g_ring_count; i++) {
    const float t = g_ring_t[i];
    if (isnan(tmin) || t < tmin)
      tmin = t;
    if (isnan(tmax) || t > tmax)
      tmax = t;
    tsum += t;
  }
  char buf[288];
  snprintf(buf, sizeof(buf),
           "{\"run_today_s\":%lu,\"run_total_s\":%lu,\"energy_wh\":%.0f,"
           "\"watts_now\":%.0f,\"t_min_f\":%.1f,\"t_max_f\":%.1f,"
           "\"t_avg_f\":%.1f,\"samples\":%u}",
           (unsigned long)g_run_today_s, (unsigned long)g_run_total_s, g_energy_wh,
           fan_watts(g_speed < 0 ? 0 : g_speed), isnan(tmin) ? 0 : tmin * 9 / 5 + 32,
           isnan(tmax) ? 0 : tmax * 9 / 5 + 32,
           g_ring_count ? (tsum / g_ring_count) * 9 / 5 + 32 : 0, g_ring_count);
  g_http.send(200, "application/json", buf);
}

// Bounded raw send: WebServer's own writes go through NetworkClient::write,
// which burns up to 10 s of 1 s selects PER CALL against a peer that stops
// draining. The 21.8 KB page goes out in ~16 chunks, so one stalled browser
// meant 160 s of stall -- the 30 s task watchdog panicked mid-serve (the
// user-visible "clipped" truncated pages) and the board crash-looped. This
// path never blocks more than 50 ms at a time, feeds the watchdog between
// slices, and drops any peer that makes no progress for 4 s.
static bool send_bounded(int s, const char* p, size_t n) {
  uint32_t last_progress = millis();
  size_t off = 0;
  while (off < n) {
    fd_set set;
    timeval tv{0, 50000};
    FD_ZERO(&set);
    FD_SET(s, &set);
    const int r = select(s + 1, nullptr, &set, nullptr, &tv);
    esp_task_wdt_reset();
    if (r < 0)
      return false;
    if (r > 0 && FD_ISSET(s, &set)) {
      const int w = send(s, p + off, n - off, MSG_DONTWAIT);
      if (w > 0) {
        off += w;
        last_progress = millis();
        continue;
      }
      if (w < 0 && errno != EAGAIN)
        return false;
    }
    if (millis() - last_progress > 4000)
      return false;  // peer stalled: drop it, never stall loop()
  }
  return true;
}

static void send_big(const char* mime, const char* body, size_t len, const char* extra_hdr = "") {
  WiFiClient c = g_http.client();
  const int s = c.fd();
  if (s < 0)
    return;
  char hdr[224];
  const int hn = snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                          "%sConnection: close\r\n\r\n",
                          mime, (unsigned)len, extra_hdr);
  if (!send_bounded(s, hdr, hn) || !send_bounded(s, body, len)) {
  }
  c.stop();  // Connection: close either way; a stalled peer is already gone
}

static void handle_csv() {
  String out;
  out.reserve(g_ring_count * 44 + 64);
  out += "epoch,temp_c,rh,hpa,outside_f,speed\n";
  for (uint16_t i = 0; i < g_ring_count; i++) {
    char l[80];
    const long ts = g_ring_end_ts ? (long)g_ring_end_ts - (long)(g_ring_count - 1 - i) * 300 : 0;
    snprintf(l, sizeof(l), "%ld,%.2f,%.0f,%.1f,%.1f,%d\n", ts, g_ring_t[i], g_ring_h[i],
             g_ring_p[i], isnan(g_ring_o[i]) ? -999 : g_ring_o[i], (int)g_ring_s[i]);
    out += l;
  }
  send_big("text/csv", out.c_str(), out.length(),
           "Content-Disposition: attachment; filename=garage-fan-24h.csv\r\n");
}

static const char kManifest[] PROGMEM = R"json({
"name":"Garage fan","short_name":"GarageFan","start_url":"/",
"display":"standalone","background_color":"#0e1116","theme_color":"#0e1116",
"icons":[{"src":"/icon.svg","sizes":"any","type":"image/svg+xml"}]})json";

static const char kIcon[] PROGMEM =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
<rect width="100" height="100" rx="22" fill="#0e1116"/>
<g fill="#3b82f6"><circle cx="50" cy="50" r="9"/>
<path d="M50 14a10 10 0 0 1 10 10c0 8-6 12-8 18l-4-1c-2-9-8-13-8-19a10 10 0 0 1 10-8z"/>
<path d="M50 86a10 10 0 0 1-10-10c0-8 6-12 8-18l4 1c2 9 8 13 8 19a10 10 0 0 1-10 8z"/>
<path d="M14 50a10 10 0 0 1 10-10c8 0 12 6 18 8l-1 4c-9 2-13 8-19 8a10 10 0 0 1-8-10z"/>
<path d="M86 50a10 10 0 0 1-10 10c-8 0-12-6-18-8l1-4c9-2 13-8 19-8a10 10 0 0 1 8 10z"/>
</g></svg>)svg";

static void handle_state() {
  char buf[768];
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
  if (g_http.hasArg("offc")) {
    const float v = g_http.arg("offc").toFloat();
    if (v >= -15 && v <= 15) {
      g_off_chg = v;
      g_prefs.putFloat("offc", v);
    }
  }
  if (g_http.hasArg("offi")) {
    const float v = g_http.arg("offi").toFloat();
    if (v >= -15 && v <= 15) {
      g_off_idle = v;
      g_prefs.putFloat("offi", v);
    }
  }
  if (g_http.hasArg("min")) {
    const int m = g_http.arg("min").toInt();
    if (m >= 0 && m <= 12) {
      g_auto_min = m;
      g_prefs.putInt("amin", m);
    }
  }
  if (g_http.hasArg("onf")) {
    const float v = g_http.arg("onf").toFloat();
    if (v >= 0.5f && v <= 20) {
      g_auto_onf = v;
      g_prefs.putFloat("onf", v);
    }
  }
  if (g_http.hasArg("offf")) {
    const float v = g_http.arg("offf").toFloat();
    if (v >= 0 && v <= 20) {
      g_auto_offf = v;
      g_prefs.putFloat("offf", v);
    }
  }
  // Hysteresis needs release strictly below engage or the latch flaps.
  if (g_auto_offf >= g_auto_onf) {
    g_auto_offf = g_auto_onf > 0.5f ? g_auto_onf - 0.5f : 0;
    g_prefs.putFloat("offf", g_auto_offf);
  }
  if (g_http.hasArg("newtoken") && g_http.arg("auth") == g_token) {
    const String nt = g_http.arg("newtoken");
    if (nt.length() >= 6 && nt.length() < 39) {
      snprintf(g_token, sizeof(g_token), "%s", nt.c_str());
      g_prefs.putString("token", g_token);
      Serial.println("ota token changed");
    }
  }
  sse_push();
  handle_state();
}

static void handle_sensors() {
  if (!g_bme_ok || g_ring_count == 0) {
    g_http.send(200, "application/json", "{\"ok\":false}");
    return;
  }
  char buf[128];
  const uint16_t i = g_ring_count - 1;
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"temp_c\":%.2f,\"rh\":%.1f,\"hpa\":%.1f}", g_ring_t[i],
           g_ring_h[i], g_ring_p[i]);
  g_http.send(200, "application/json", buf);
}

static void append_series(String& out, const char* name, const float* v, uint16_t n,
                          uint8_t decimals) {
  out += '"';
  out += name;
  out += "\":[";
  char num[16];
  for (uint16_t i = 0; i < n; i++) {
    if (isnan(v[i])) {
      // A literal "nan" is not JSON -- one bad sample would make the
      // browser's parser reject the entire history payload (blank graph).
      out += "null";
    } else {
      dtostrf(v[i], 0, decimals, num);
      out += num;
    }
    if (i + 1 < n)
      out += ',';
  }
  out += ']';
}

// Stream the month files covering [cutoff, now], decimating by stride into
// the caller's arrays. Two passes: count, then collect every (count/max)th.
static uint16_t sd_read_range(time_t cutoff, float* t, float* h, float* p, uint16_t max_pts) {
  CRUMB("sd_read");
  time_t now = time(nullptr);
  char paths[2][24];
  int npaths = 0;
  for (time_t at = cutoff; npaths < 2; at = now) {
    struct tm tmv;
    gmtime_r(&at, &tmv);
    char path[24];
    snprintf(path, sizeof(path), "/climate-%04d%02d.csv", tmv.tm_year + 1900, tmv.tm_mon + 1);
    if (npaths == 0 || strcmp(path, paths[0]) != 0)
      snprintf(paths[npaths++], 24, "%s", path);
    if (at == now)
      break;
  }
  uint32_t rows = 0;
  for (int pass = 0; pass < 2; pass++) {
    const uint32_t stride = pass ? (rows > max_pts ? rows / max_pts + 1 : 1) : 1;
    uint32_t seen = 0;
    uint16_t kept = 0;
    for (int i = 0; i < npaths; i++) {
      File f = SD.open(paths[i], FILE_READ);
      if (!f)
        continue;
      // Line-buffered scan; lines are short and epoch-prefixed.
      char line[96];
      size_t ll = 0;
      while (f.available()) {
        const char c = (char)f.read();
        if (c != '\n') {
          if (ll < sizeof(line) - 1)
            line[ll++] = c;
          continue;
        }
        line[ll] = '\0';
        ll = 0;
        const long epoch = strtol(line, nullptr, 10);
        if (epoch < cutoff)
          continue;
        if (pass == 0) {
          rows++;
          continue;
        }
        if (seen++ % stride)
          continue;
        if (kept >= max_pts)
          break;
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
    if (pass == 1) {
      CRUMB_CLEAR();
      return kept;
    }
    if (rows == 0) {
      CRUMB_CLEAR();
      return 0;
    }
  }
  CRUMB_CLEAR();
  return 0;
}

static void handle_history() {
  const int days = g_http.hasArg("days") ? g_http.arg("days").toInt() : 1;
  String out;
  out.reserve(13000);
  if (days <= 1 || !g_sd_ok || !time_synced()) {
    char hd[64];
    snprintf(hd, sizeof(hd), "{\"interval_s\":300,\"end_ts\":%ld,", (long)g_ring_end_ts);
    out += hd;
    append_series(out, "temp_c", g_ring_t, g_ring_count, 1);
    out += ',';
    append_series(out, "rh", g_ring_h, g_ring_count, 0);
    out += ',';
    append_series(out, "hpa", g_ring_p, g_ring_count, 1);
    out += ',';
    append_series(out, "out_f", g_ring_o, g_ring_count, 1);
    out += ',';
    append_series(out, "batt_v", g_ring_bv, g_ring_count, 2);
    out += ",\"spd\":[";
    for (uint16_t i = 0; i < g_ring_count; i++) {
      char n[6];
      snprintf(n, sizeof(n), "%d", (int)g_ring_s[i]);
      out += n;
      if (i + 1 < g_ring_count)
        out += ',';
    }
    out += "],\"chg\":[";
    for (uint16_t i = 0; i < g_ring_count; i++) {
      char n[6];
      snprintf(n, sizeof(n), "%d", (int)g_ring_c[i]);
      out += n;
      if (i + 1 < g_ring_count)
        out += ',';
    }
    out += "]}";
  } else {
    static float t[kGraphMaxPts], h[kGraphMaxPts], p[kGraphMaxPts];
    const time_t cutoff = time(nullptr) - (time_t)days * 86400;
    const uint16_t n = sd_read_range(cutoff, t, h, p, kGraphMaxPts);
    char head[48];
    snprintf(head, sizeof(head), "{\"interval_s\":%ld,", n > 1 ? (long)(days * 86400L / n) : 300L);
    out += head;
    append_series(out, "temp_c", t, n, 1);
    out += ',';
    append_series(out, "rh", h, n, 0);
    out += ',';
    append_series(out, "hpa", p, n, 1);
    out += '}';
  }
  send_big("application/json", out.c_str(), out.length());
}

// Token-guarded one-shot: mount with format-on-failure, turning a fresh
// exFAT card into FAT32 in place. Deliberately NOT automatic on normal
// mounts -- a flaky-but-full card must never be silently erased. Blocks the
// loop for the duration (can be minutes on big cards); the RMT peripheral
// keeps the fan running throughout.
static void handle_sd_format() {
  if (g_http.arg("token") != g_token) {
    g_http.send(403, "application/json", "{\"error\":\"bad token\"}");
    return;
  }
  Serial.println("[SD] format requested");
  esp_task_wdt_delete(NULL);  // a big-card format legitimately takes minutes
  g_sd_quarantined = false;   // manual override un-quarantines
  rtc_sd_sentinel = kSdSentinelMagic;
  CRUMB("sd_format");
  SD.end();
  SPI.end();
  delay(50);
  SPI.begin();
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  pinMode(SRAM_CS_PIN, OUTPUT);
  digitalWrite(SRAM_CS_PIN, HIGH);
  pinMode(EPD_CS_PIN, OUTPUT);
  digitalWrite(EPD_CS_PIN, HIGH);
  const bool ok = SD.begin(SD_CS_PIN, SPI, 4000000, "/sd", 5, true);
  rtc_sd_sentinel = 0;
  CRUMB_CLEAR();
  esp_task_wdt_add(NULL);
  g_sd_ok = ok;
  g_sd_fails = 0;
  char buf[80];
  snprintf(buf, sizeof(buf), "{\"ok\":%s,\"total_mb\":%lu}", ok ? "true" : "false",
           ok ? (unsigned long)(SD.totalBytes() / 1048576) : 0);
  Serial.printf("[SD] format result: %s\n", buf);
  g_http.send(ok ? 200 : 500, "application/json", buf);
}

// Raw SD probe: bit-level CMD0/CMD8 handshake at 400 kHz, reporting each
// step, so "card not seated", "card dead", and "card incompatible" stop
// looking identical. Read-only; safe on any card.
static void handle_sd_test() {
  SD.end();
  SPI.end();
  delay(50);
  SPI.begin();
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  pinMode(SRAM_CS_PIN, OUTPUT);
  digitalWrite(SRAM_CS_PIN, HIGH);
  pinMode(EPD_CS_PIN, OUTPUT);
  digitalWrite(EPD_CS_PIN, HIGH);
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 10; i++) SPI.transfer(0xFF);  // 80 warm-up clocks
  digitalWrite(SD_CS_PIN, LOW);
  static const uint8_t kCmd0[] = {0x40, 0, 0, 0, 0, 0x95};
  for (uint8_t b : kCmd0) SPI.transfer(b);
  uint8_t r1 = 0xFF;
  for (int i = 0; i < 16 && (r1 & 0x80); i++) r1 = SPI.transfer(0xFF);
  uint8_t r7[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t r1b = 0xFF;
  if (r1 == 0x01) {  // idle: try CMD8 (SDv2 voltage check, echoes 0x1AA)
    static const uint8_t kCmd8[] = {0x48, 0, 0, 0x01, 0xAA, 0x87};
    for (uint8_t b : kCmd8) SPI.transfer(b);
    for (int i = 0; i < 16 && (r1b & 0x80); i++) r1b = SPI.transfer(0xFF);
    if (r1b == 0x01)
      for (int i = 0; i < 4; i++) r7[i] = SPI.transfer(0xFF);
  }
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.transfer(0xFF);
  SPI.endTransaction();
  char buf[160];
  snprintf(buf, sizeof(buf),
           "{\"cmd0_r1\":\"0x%02X\",\"cmd8_r1\":\"0x%02X\","
           "\"cmd8_echo\":\"%02X%02X%02X%02X\",\"verdict\":\"%s\"}",
           r1, r1b, r7[0], r7[1], r7[2], r7[3],
           r1 == 0xFF   ? "no response - card absent, unseated, or bad contact"
           : r1 == 0x01 ? (r7[2] == 0x01 && r7[3] == 0xAA ? "card alive, SDv2, handshake ok"
                                                          : "card alive but CMD8 odd")
                        : "card answered abnormally");
  Serial.printf("[SD] probe: %s\n", buf);
  g_http.send(200, "application/json", buf);
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
  if (g_speed == -1 && v == 0)
    g_speed = -2;  // force off to re-apply after raw
  apply_speed(v, "http");
  handle_state();
}

static void handle_update_upload() {
  HTTPUpload& up = g_http.upload();
  if (up.status == UPLOAD_FILE_START) {
    g_ota_authorized = g_http.arg("token") == g_token;
    if (!g_ota_authorized) {
      Serial.println("[OTA] rejected: bad token");
      return;
    }
    Serial.printf("[OTA] receiving %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN))
      Update.printError(Serial);
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
  g_http.send(200, "application/json", "{\"ok\":true,\"note\":\"rebooting into new slot\"}");
  delay(300);
  esp_restart();
}

static void on_message(char* topic, uint8_t* payload, unsigned int len) {
  char buf[16];
  if (len >= sizeof(buf))
    return;
  memcpy(buf, payload, len);
  buf[len] = '\0';
  if (strstr(topic, "/ts") != nullptr &&
      strncmp(topic, MQTT_SUB_BASE, strlen(MQTT_SUB_BASE)) == 0) {
    const long e = strtol(buf, nullptr, 10);
    if (e > 1700000000)
      g_outdoor_epoch = e;
    return;
  }
  if (strcmp(topic, kTopicOutdoor) == 0) {
    char* end = nullptr;
    const float f = strtof(buf, &end);
    if (end != buf && isfinite(f) && f > -60 && f < 150) {
      g_outside_c = (f - 32.0f) * 5.0f / 9.0f;
      g_outside_ms = millis();
    }
    return;
  }
  if (strcmp(topic, kTopicSet) != 0 || len == 0 || len > 2)
    return;
  char* end = nullptr;
  long v = strtol(buf, &end, 10);
  if (end == buf || *end != '\0')
    return;
  apply_speed(static_cast<int>(v), "mqtt");
  publish_state();
}

static void ensure_mqtt() {
  if (WiFi.status() != WL_CONNECTED)
    return;
  if (g_mqtt.connected() || millis() - g_last_reconnect_ms < 3000)
    return;
  g_last_reconnect_ms = millis();
  char id[24];
  snprintf(id, sizeof(id), "garage-fan-%06llx", ESP.getEfuseMac() & 0xffffff);
  if (g_mqtt.connect(id, MQTT_USER, MQTT_PASS, kTopicAvail, 0, true, "offline")) {
    Serial.println("mqtt connected");
    g_mqtt_up_ms = millis();
    g_ever_healthy = true;
    ota_rollback_mark_healthy();
    g_mqtt.publish(kTopicAvail, "online", true);
    publish_state();
    g_mqtt.subscribe(kTopicSet);
    g_mqtt.subscribe(kTopicOutdoor);
    g_mqtt.subscribe(MQTT_SUB_BASE "/ts");
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
  g_off_chg = g_prefs.getFloat("offc", -3.0f);
  g_off_idle = g_prefs.getFloat("offi", -1.0f);
  g_auto_min = g_prefs.getInt("amin", 0);
  g_auto_onf = g_prefs.getFloat("onf", 2.5f);
  g_auto_offf = g_prefs.getFloat("offf", 1.5f);
  g_chg = g_prefs.getBool("chg", false);
  g_run_total_s = g_prefs.getUInt("runs", 0);
  g_run_today_s = g_prefs.getUInt("runt", 0);
  g_today_ymd = g_prefs.getUInt("ymd", 0);
  g_energy_wh = g_prefs.getFloat("ewh", 0);
  String tk = g_prefs.getString("token", FAN_OTA_TOKEN);
  snprintf(g_token, sizeof(g_token), "%s", tk.c_str());
  const int saved = g_prefs.getInt("speed", 0);
  if (saved > 0 && saved <= 12) {
    g_speed = saved;
    set_wave(kHighUs[saved]);  // resume before WiFi even exists
    Serial.printf("restored speed %d from nvs\n", saved);
  }
  // SD stays OUT of the boot path: the controller comes fully online first,
  // and the first mount attempt happens ~60 s later from loop(). A card that
  // crashes the mount gets quarantined by the sentinel above -- no card can
  // take fan control down with it.
  const esp_reset_reason_t rr = esp_reset_reason();
  const bool abnormal = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
                        rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT;
  snprintf(g_last_death, sizeof(g_last_death), "%s%s%s", reset_reason_str(rr),
           rtc_crumb[0] ? " during " : "", rtc_crumb[0] ? rtc_crumb : "");
  // Quarantine on a mount sentinel from any reset, or any abnormal death
  // while an SD op was in flight.
  g_sd_quarantined =
      (rtc_sd_sentinel == kSdSentinelMagic) || (abnormal && strncmp(rtc_crumb, "sd", 2) == 0);
  rtc_sd_sentinel = 0;
  CRUMB_CLEAR();
  if (g_sd_quarantined)
    Serial.println("[SD] previous boot died in an SD op -- card quarantined");
  Serial.printf("last reset: %s\n", g_last_death);
  // Longitudinal forensics: RTC evidence dies with power, NVS doesn't. Keep
  // the prior boot's certificate and a lifetime boot counter so a reboot
  // storm is visible from /api/state even after the fact.
  {
    String pd = g_prefs.getString("pdeath", "none");
    snprintf(g_prev_death, sizeof(g_prev_death), "%s", pd.c_str());
    g_prefs.putString("pdeath", g_last_death);
    g_boots = g_prefs.getUInt("boots", 0) + 1;
    g_prefs.putUInt("boots", g_boots);
  }
  // Task watchdog: a frozen loop (hung bus op, wedged driver) force-reboots
  // in 30 s instead of hanging dark forever. The fan rides through on RMT.
  // 60 s: tolerates worst-case framework writes to one stalled peer (10 s
  // per NetworkClient::write call) on the small JSON endpoints while still
  // catching genuine hangs. Large payloads use send_bounded and never stall.
  esp_task_wdt_config_t wdt_cfg = {.timeout_ms = 60000, .idle_core_mask = 0, .trigger_panic = true};
  if (esp_task_wdt_init(&wdt_cfg) != ESP_OK)
    esp_task_wdt_reconfigure(&wdt_cfg);
  esp_task_wdt_add(NULL);
  Wire.begin();
  SPI.begin();
  CRUMB("epd_init");
  g_epd.begin();
  g_epd.setRotation(1);
  g_epd_ok = true;
  CRUMB_CLEAR();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(FAN_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org");
  g_mqtt.setServer(MQTT_HOST, MQTT_PORT);
  g_mqtt.setCallback(on_message);
  g_http.on("/", []() { send_big("text/html", kPage, sizeof(kPage) - 1); });
  g_http.on("/api/state", handle_state);
  g_http.on("/api/set", handle_set);
  g_http.on("/api/raw", handle_raw);
  g_http.on("/api/config", handle_config);
  g_http.on("/api/sensors", handle_sensors);
  g_http.on("/api/history", handle_history);
  g_http.on("/api/stats", handle_stats);
  g_http.on("/download.csv", handle_csv);
  g_http.on("/manifest.json",
            []() { send_big("application/json", kManifest, sizeof(kManifest) - 1); });
  g_http.on("/icon.svg", []() { send_big("image/svg+xml", kIcon, sizeof(kIcon) - 1); });
  g_http.on("/api/sdformat", handle_sd_format);
  g_http.on("/api/battdebug", []() {
    char out[512];
    const bool w15 = lc_write(0x15, 0x0001);
    const bool w0b = lc_write(0x0B, 0x0056);
    String r;
    for (int variant = 0; variant < 2; variant++) {
      for (uint8_t reg : {(uint8_t)0x09, (uint8_t)0x0D, (uint8_t)0x11}) {
        Wire.beginTransmission(0x0B);
        Wire.write(reg);
        const int wtx = Wire.endTransmission(variant == 1);
        int got = 0;
        uint8_t raw[3] = {0, 0, 0};
        if (wtx == 0) {
          got = Wire.requestFrom((uint8_t)0x0B, (uint8_t)3);
          for (int i = 0; i < got && i < 3; i++) raw[i] = Wire.read();
        }
        const uint8_t chk[5] = {0x16, reg, 0x17, raw[0], raw[1]};
        char e[96];
        snprintf(e, sizeof(e),
                 "{\"reg\":\"0x%02X\",\"stop\":%d,\"wtx\":%d,\"got\":%d,"
                 "\"bytes\":\"%02X%02X%02X\",\"val\":%u,\"crc_ok\":%s},",
                 reg, variant, wtx, got, raw[0], raw[1], raw[2],
                 (unsigned)(((uint16_t)raw[1] << 8) | raw[0]),
                 lc_crc(chk, 5) == raw[2] ? "true" : "false");
        r += e;
      }
    }
    snprintf(out, sizeof(out), "{\"w15\":%s,\"w0b\":%s,\"reads\":[", w15 ? "true" : "false",
             w0b ? "true" : "false");
    String full = String(out) + r.substring(0, r.length() - 1) + "]}";
    g_http.send(200, "application/json", full);
  });
  g_http.on("/api/i2cscan", []() {
    String out = "{\"found\":[";
    bool first = true;
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        if (!first)
          out += ',';
        char b[8];
        snprintf(b, sizeof(b), "\"0x%02X\"", a);
        out += b;
        first = false;
      }
    }
    out += "]}";
    g_http.send(200, "application/json", out);
  });
  g_http.on("/api/sdtest", handle_sd_test);
  g_http.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
  g_http.onNotFound([]() { g_http.send(404, "application/json", "{\"error\":\"404\"}"); });
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
        g_sse_srv.begin();
        Serial.printf("web ui: http://%s.local/\n", FAN_HOSTNAME);
      }
    }
  }
  esp_task_wdt_reset();
  ensure_mqtt();
  g_mqtt.loop();
  g_http.handleClient();
  sse_accept();
  // Runtime odometer + energy integration, once per second.
  if (millis() - g_last_count_ms >= 1000) {
    g_last_count_ms = millis();
    if (g_speed > 0) {
      g_run_total_s++;
      g_run_today_s++;
      g_energy_wh += fan_watts(g_speed) / 3600.0f;
    }
    if (time_synced()) {
      struct tm lt;
      time_t now = time(nullptr);
      gmtime_r(&now, &lt);
      const uint32_t ymd = (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
      if (g_today_ymd != 0 && ymd != g_today_ymd)
        g_run_today_s = 0;
      g_today_ymd = ymd;
    }
  }
  if (millis() - g_last_nvs_ms >= 15 * 60 * 1000) {
    g_last_nvs_ms = millis();
    g_prefs.putUInt("runs", g_run_total_s);
    g_prefs.putUInt("runt", g_run_today_s);
    g_prefs.putUInt("ymd", g_today_ymd);
    g_prefs.putFloat("ewh", g_energy_wh);
  }
  // E-ink: refresh with each sample cycle, or 60 s after a speed change.
  if (g_epd_ok && g_ring_count &&
      ((millis() - g_last_epd_ms >= kSampleMs) ||
       (g_speed != g_epd_speed_shown && millis() - g_last_epd_ms >= 60000)))
    epd_render();
  if (g_last_sample_ms == 0 || millis() - g_last_sample_ms >= kSampleMs) {
    g_last_sample_ms = millis();
    sample_climate();
    sse_push();
  }
  if (millis() - g_last_auto_ms >= kAutoTickMs) {
    g_last_auto_ms = millis();
    auto_tick();
    batt_begin();
    batt_read();
    // First mount attempt ~60 s after boot, then every 5 min, 10-fail cap,
    // and never while quarantined; /api/sdformat overrides all of it.
    static bool sd_tried = false;
    static uint8_t sd_backoff = 0;
    if (!g_sd_ok && !g_sd_quarantined && g_sd_fails < 10) {
      const bool due = sd_tried ? (++sd_backoff >= 10) : (millis() > 60000);
      if (due) {
        sd_tried = true;
        sd_backoff = 0;
        sd_mount_guarded();
        g_sd_fails = g_sd_ok ? 0 : g_sd_fails + 1;
      }
    }
  }
  if (!g_ever_healthy && !ota_rollback_image_confirmed() && millis() > kUnconfirmedDeadlineMs) {
    Serial.println("[OTA] unconfirmed image never reached broker; restarting");
    Serial.flush();
    esp_restart();
  }
  delay(2);
}
