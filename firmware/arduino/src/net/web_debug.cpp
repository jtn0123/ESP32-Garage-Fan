// See web_debug.h. Bodies verbatim from the pre-split firmware.
#include "net/web_debug.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>

#include <cstdio>

#include "config.h"
#include "fan/control.h"
#include "net/http_tx.h"
#include "net/plug.h"
#include "sensors/battery.h"
#include "storage/sdcard.h"

namespace web_debug {
namespace {

WebServer* g_http = nullptr;
const char* g_token = "";

bool authorized() {
  // Same rule as web.cpp's token_ok: an empty configured token authorizes
  // nothing -- otherwise a missing ?token= argument compares "" == "".
  if (g_token[0] != '\0' && g_http->arg("token") == g_token)
    return true;
  g_http->send(403, "application/json", "{\"error\":\"bad token\"}");
  return false;
}

// One raw SPI-mode command; returns R1 (0xFF = no answer within 16 clocks).
uint8_t sd_cmd(uint8_t cmd, uint32_t arg, uint8_t crc) {
  SPI.transfer(0x40 | cmd);
  SPI.transfer((arg >> 24) & 0xFF);
  SPI.transfer((arg >> 16) & 0xFF);
  SPI.transfer((arg >> 8) & 0xFF);
  SPI.transfer(arg & 0xFF);
  SPI.transfer(crc);
  uint8_t r1 = 0xFF;
  for (int i = 0; i < 16 && (r1 & 0x80); i++) r1 = SPI.transfer(0xFF);
  return r1;
}

// Raw SD probe: the FULL SPI-mode init conversation at 400 kHz -- CMD0/CMD8
// handshake, the ACMD41 wake-up loop, OCR, CSD, and an actual read of block 0
// -- reporting each stage, so "card not seated", "card dead", "init stalls"
// and "reads corrupt" stop looking identical. Read-only; safe on any card.
// Exists because a healthy Mac-verified FAT32 card handshakes here but
// SD.begin() fails at every bus speed (2026-08-09) and nothing said why.
void handle_sd_test() {
  if (!authorized())
    return;
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

  const uint8_t r1_cmd0 = sd_cmd(0, 0, 0x95);
  uint8_t r7[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t r1_cmd8 = 0xFF;
  if (r1_cmd0 == 0x01) {  // idle: CMD8 (SDv2 voltage check, echoes 0x1AA)
    r1_cmd8 = sd_cmd(8, 0x000001AA, 0x87);
    if (r1_cmd8 == 0x01) {
      for (int i = 0; i < 4; i++) r7[i] = SPI.transfer(0xFF);
    }
  }

  // ACMD41 wake-up loop with HCS: this is where marginal power dies -- the
  // card draws its init current here and never leaves idle.
  uint8_t r1_acmd41 = 0xFF;
  int iters = 0;
  const uint32_t t0 = millis();
  if (r1_cmd8 == 0x01) {
    while (millis() - t0 < 1000) {
      sd_cmd(55, 0, 0xFF);
      r1_acmd41 = sd_cmd(41, 0x40000000, 0xFF);
      iters++;
      if (r1_acmd41 == 0x00)
        break;
      delay(10);
    }
  }
  const uint32_t acmd_ms = millis() - t0;

  uint8_t ocr[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t r1_cmd58 = 0xFF;
  if (r1_acmd41 == 0x00) {
    r1_cmd58 = sd_cmd(58, 0, 0xFF);
    if (r1_cmd58 <= 0x01) {
      for (int i = 0; i < 4; i++) ocr[i] = SPI.transfer(0xFF);
    }
  }

  // Block 0 read: the decisive stage. If this returns the MBR signature the
  // bus, power and card are all fine and the failure is software-side.
  uint8_t r1_cmd17 = 0xFF, token = 0xFF;
  uint8_t first8[8] = {0};
  bool mbr_sig = false;
  if (r1_cmd58 <= 0x01) {
    r1_cmd17 = sd_cmd(17, 0, 0xFF);
    if (r1_cmd17 == 0x00) {
      for (int i = 0; i < 2000 && token == 0xFF; i++) token = SPI.transfer(0xFF);
      if (token == 0xFE) {
        uint8_t sig0 = 0, sig1 = 0;
        for (int i = 0; i < 512; i++) {
          const uint8_t b = SPI.transfer(0xFF);
          if (i < 8)
            first8[i] = b;
          if (i == 510)
            sig0 = b;
          if (i == 511)
            sig1 = b;
        }
        SPI.transfer(0xFF);  // discard the 2 CRC bytes
        SPI.transfer(0xFF);
        mbr_sig = (sig0 == 0x55 && sig1 == 0xAA);
      }
    }
  }
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.transfer(0xFF);
  SPI.endTransaction();

  const char* verdict = r1_cmd0 == 0xFF   ? "no response - card absent, unseated, or bad contact"
                        : r1_cmd0 != 0x01 ? "CMD0 abnormal - card answered but not idle"
                        : r1_cmd8 != 0x01 ? "CMD8 rejected - SDv1 or protocol trouble"
                        : r1_acmd41 != 0x00
                            ? "init stalls at ACMD41 - power sag or card incompatibility"
                        : r1_cmd58 > 0x01 ? "OCR read failed after init - bus integrity"
                        : token != 0xFE   ? "block read token missing - bus or card data path"
                        : mbr_sig ? "FULL INIT + BLOCK READ OK - failure is in the SD library layer"
                                  : "block read corrupt - MISO integrity";

  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\"cmd0\":\"0x%02X\",\"cmd8\":\"0x%02X\",\"echo\":\"%02X%02X%02X%02X\","
           "\"acmd41\":{\"r1\":\"0x%02X\",\"iters\":%d,\"ms\":%lu},"
           "\"ocr\":\"%02X%02X%02X%02X\",\"ccs\":%s,"
           "\"read0\":{\"r1\":\"0x%02X\",\"token\":\"0x%02X\",\"mbr_sig\":%s,"
           "\"first8\":\"%02X%02X%02X%02X%02X%02X%02X%02X\"},"
           "\"verdict\":\"%s\"}",
           r1_cmd0, r1_cmd8, r7[0], r7[1], r7[2], r7[3], r1_acmd41, iters, (unsigned long)acmd_ms,
           ocr[0], ocr[1], ocr[2], ocr[3], (ocr[0] & 0x40) ? "true" : "false", r1_cmd17, token,
           mbr_sig ? "true" : "false", first8[0], first8[1], first8[2], first8[3], first8[4],
           first8[5], first8[6], first8[7], verdict);
  Serial.printf("[SD] probe: %s\n", buf);
  // The probe tore down the SD/SPI buses; tell the owning module and remount
  // so the next 5-minute sample does not silently drop its CSV row.
  sdcard::mark_unmounted();
  sdcard::mount_guarded();
  g_http->send(200, "application/json", buf);
}

}  // namespace

void register_routes(WebServer& http, const char* token) {
  g_http = &http;
  g_token = token;
  http.on("/api/sdtest", handle_sd_test);
  // Every device answering on the I2C bus. Exists for sensor bring-up: the
  // SHT41 (0x44) and SGP41 (0x59) arrived on a STEMMA chain 2026-08-13 and
  // the first question is always "does the bus even see them".
  http.on("/api/i2cscan", []() {
    if (!authorized())
      return;
    char out[256];
    size_t n = snprintf(out, sizeof(out), "{\"found\":[");
    bool first = true;
    for (uint8_t a = 0x08; a < 0x78; ++a) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0 && n < sizeof(out) - 12)
        n += snprintf(out + n, sizeof(out) - n, "%s\"0x%02x\"", first ? "" : ",", a), first = false;
    }
    snprintf(out + n, sizeof(out) - n, "]}");
    g_http->send(200, "application/json", out);
  });
  // Read the fan pin's own pad while LEDC drives it. Exists because on
  // 2026-08-13 the fan ignored a whole calibration sweep, rmtWriteLooping
  // reported success, and nothing could say whether the waveform physically
  // left the chip. The probe itself lives in fan::probe_pad now, so the
  // cycling profile can log the same reading unattended.
  http.on("/api/pinprobe", []() {
    if (!authorized())
      return;
    float high_pct = 0;
    uint32_t transitions = 0;
    fan::probe_pad(&high_pct, &transitions);
    char out[128];
    snprintf(out, sizeof(out), "{\"high_pct\":%.1f,\"transitions\":%lu,\"want_high_us\":%u}",
             static_cast<double>(high_pct), (unsigned long)transitions, fan::commanded_high_us());
    g_http->send(200, "application/json", out);
  });
  // Every raw meter poll the device still remembers: 15 minutes of 15 s
  // samples, streamed rather than summarised. The flight recorder holds 24
  // lines and a fan can cycle faster than one line per five minutes, so the
  // shape of an episode cannot live on the tape -- it lives here, costs
  // nothing until asked for, and is what turns "the meter looks noisy" into
  // a sequence anyone can read.
  http.on("/api/plugtrace", []() {
    if (!authorized())
      return;
    const uint8_t n = plug::trace_count();
    http_tx::Chunked tx(g_http->client(), "application/json");
    tx.printf("{\"poll_s\":%u,\"n\":%u,\"w\":[", static_cast<unsigned>(plug::poll_interval_s()),
              static_cast<unsigned>(n));
    for (uint8_t i = 0; i < n && tx.ok(); i++) {
      const plug::Reading r = plug::trace_at(i);
      char num[16];
      if (r.dw == plug::Reading::kNoRead) {
        snprintf(num, sizeof(num), "null");
      } else {
        // Tenths straight out of the encoding: no float formatting, so a
        // corrupt sample cannot render a hundred digits into the stream.
        snprintf(num, sizeof(num), "%u.%u", static_cast<unsigned>(r.dw / 10),
                 static_cast<unsigned>(r.dw % 10));
      }
      tx.print(num);
      tx.print(i + 1 < n ? "," : "");
    }
    tx.print("],\"spd\":[");
    for (uint8_t i = 0; i < n && tx.ok(); i++)
      tx.printf(i + 1 < n ? "%d," : "%d", static_cast<int>(plug::trace_at(i).speed));
    tx.print("],\"cls\":[");
    for (uint8_t i = 0; i < n && tx.ok(); i++)
      tx.printf(i + 1 < n ? "%d," : "%d", static_cast<int>(plug::trace_at(i).cls));
    tx.print("]}");
    tx.end();
  });
  http.on("/api/battdebug", []() {
    if (!authorized())
      return;
    char out[512];
    // Read-only on purpose. This used to write APA register 0x0B = 0x0056
    // before probing -- a DIFFERENT pack profile than the 0x0055 (2x3200 =
    // 6400 mAh) that battery::begin() selects. Opening this endpoint once
    // therefore re-calibrated the gauge's RSOC until the next reboot, so every
    // percentage, the drain slope, the sticky charging verdict and the
    // BME280 self-heating offset that depends on it all ran off a profile the
    // firmware never chose. A probe must not perturb the state it reports.
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
                 static_cast<unsigned>((static_cast<uint16_t>(raw[1]) << 8) | raw[0]),
                 battery::crc8(chk, 5) == raw[2] ? "true" : "false");
        r += e;
      }
    }
    snprintf(out, sizeof(out), "{\"read_only\":true,\"reads\":[");
    String full = String(out) + r.substring(0, r.length() - 1) + "]}";
    g_http->send(200, "application/json", full);
  });
}

}  // namespace web_debug
