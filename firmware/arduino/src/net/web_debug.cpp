// See web_debug.h. Bodies verbatim from the pre-split firmware.
#include "net/web_debug.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>

#include <cstdio>

#include "config.h"
#include "sensors/battery.h"

namespace web_debug {
namespace {

WebServer* g_http = nullptr;

// Raw SD probe: bit-level CMD0/CMD8 handshake at 400 kHz, reporting each
// step, so "card not seated", "card dead", and "card incompatible" stop
// looking identical. Read-only; safe on any card.
void handle_sd_test() {
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
  g_http->send(200, "application/json", buf);
}

}  // namespace

void register_routes(WebServer& http) {
  g_http = &http;
  http.on("/api/sdtest", handle_sd_test);
  http.on("/api/battdebug", []() {
    char out[512];
    const bool w15 = battery::write_reg(0x15, 0x0001);
    const bool w0b = battery::write_reg(0x0B, 0x0056);
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
                 battery::crc8(chk, 5) == raw[2] ? "true" : "false");
        r += e;
      }
    }
    snprintf(out, sizeof(out), "{\"w15\":%s,\"w0b\":%s,\"reads\":[", w15 ? "true" : "false",
             w0b ? "true" : "false");
    String full = String(out) + r.substring(0, r.length() - 1) + "]}";
    g_http->send(200, "application/json", full);
  });
  http.on("/api/i2cscan", []() {
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
    g_http->send(200, "application/json", out);
  });
}

}  // namespace web_debug
