// See sse.h. Transport rules verbatim from the pre-split firmware.
#include "net/sse.h"

#include <Arduino.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "lwip/sockets.h"

namespace sse {
namespace {

WiFiServer g_srv(8081);
WiFiClient g_peers[4];
StateSource g_source = nullptr;

// Never block on an SSE peer. NetworkClient::write retries a 1 s select up
// to 10 times per call, so one sleeping laptop with a full socket buffer
// costs 10 s per print -- sse_push's three prints hit the 30 s task
// watchdog and panic the board (proven live 2026-08-05: crash loop after
// v1.11.0 with several dashboards open). Instead: zero-timeout select +
// MSG_DONTWAIT send, and any peer that can't take the whole frame right
// now is dropped -- the browser's EventSource auto-reconnects.
void sse_send(WiFiClient& c, const char* buf, int n) {
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

// Read the client's request headers so accept() can see the Origin.
//
// The stream used to answer `Access-Control-Allow-Origin: *`, which let any
// web page the operator happened to have open subscribe to this port and read
// the device's whole state stream. Simply dropping the header does not work:
// the console is served from port 80 and this server listens on 8081, so the
// two are cross-origin and EventSource would be refused -- the console would
// silently fall back to the 15 s poll and feel broken.
//
// So: read the request, and echo the Origin back only when it is this device.
// Bounded hard, because this runs inside loop().
bool read_request(WiFiClient& c, char* out, size_t cap) {
  size_t n = 0;
  const uint32_t deadline = millis() + 50;
  while (millis() < deadline && n + 1 < cap) {
    const int avail = c.available();
    if (avail <= 0) {
      if (!c.connected())
        break;
      delay(1);
      continue;
    }
    const int got = c.read(reinterpret_cast<uint8_t*>(out + n), cap - 1 - n);
    if (got <= 0)
      break;
    n += static_cast<size_t>(got);
    out[n] = '\0';
    if (strstr(out, "\r\n\r\n"))
      return true;
  }
  out[n] = '\0';
  return n > 0;
}

// Case-insensitive search for a header value. Returns nullptr when absent.
const char* header_value(const char* req, const char* name, size_t* len_out) {
  for (const char* p = req; *p; p++) {
    if (strncasecmp(p, name, strlen(name)) != 0)
      continue;
    if (p != req && *(p - 1) != '\n')
      continue;  // must start a line, so "X-Origin:" cannot match "Origin:"
    const char* v = p + strlen(name);
    while (*v == ' ' || *v == '\t')
      v++;
    const char* e = v;
    while (*e && *e != '\r' && *e != '\n')
      e++;
    *len_out = static_cast<size_t>(e - v);
    return v;
  }
  return nullptr;
}

/**
 * Is `origin` this device serving its own console?
 *
 * Accepts the IP and the mDNS/host names on the default port, which are the
 * only ways the page is reachable. A request with NO Origin at all (curl,
 * `mosquitto_sub`-style debugging, any non-browser client) is allowed through:
 * CORS is a browser-enforced policy and withholding the header from a
 * non-browser protects nobody while breaking bench debugging.
 */
bool origin_is_self(const char* origin, size_t len) {
  char self[3][64];
  const String ip = WiFi.localIP().toString();
  snprintf(self[0], sizeof(self[0]), "http://%s", ip.c_str());
  snprintf(self[1], sizeof(self[1]), "http://%s", FAN_HOSTNAME);
  snprintf(self[2], sizeof(self[2]), "http://%s.local", FAN_HOSTNAME);
  for (const auto& s : self) {
    if (strlen(s) == len && strncasecmp(origin, s, len) == 0)
      return true;
  }
  return false;
}

}  // namespace

void push() {
  if (!g_source)
    return;
  char st[768];
  char frame[832];
  g_source(st, sizeof(st));
  const int n = snprintf(frame, sizeof(frame), "data: %s\n\n", st);
  if (n < 0 || n >= static_cast<int>(sizeof(frame)))
    return;  // snprintf reports intended length; a truncated frame is torn JSON
  for (auto& c : g_peers) {
    if (c && c.connected())
      sse_send(c, frame, n);
  }
}

void accept() {
  WiFiClient nc = g_srv.accept();
  if (!nc)
    return;

  // Decide the CORS answer before taking a slot, so a rejected caller never
  // occupies one of the four.
  char req[512];
  char allow[96] = {0};
  if (read_request(nc, req, sizeof(req))) {
    size_t olen = 0;
    const char* origin = header_value(req, "Origin:", &olen);
    if (origin) {
      if (!origin_is_self(origin, olen)) {
        nc.stop();  // a page that is not our console; do not stream to it
        return;
      }
      snprintf(allow, sizeof(allow), "Access-Control-Allow-Origin: %.*s\r\n",
               static_cast<int>(olen), origin);
    }
  }

  for (auto& c : g_peers) {
    if (!c || !c.connected()) {
      c = nc;
      c.setNoDelay(true);
      if (!g_source)
        return;
      char st[768];
      // Headroom for the echoed Origin header on top of the 768-byte state.
      char frame[1152];
      g_source(st, sizeof(st));
      const int n = snprintf(frame, sizeof(frame),
                             "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                             "%s"  // echoed Access-Control-Allow-Origin, or nothing
                             "Cache-Control: no-cache\r\n"
                             "Connection: keep-alive\r\n\r\nretry: 3000\n\ndata: %s\n\n",
                             allow, st);
      if (n < 0 || n >= static_cast<int>(sizeof(frame)))
        return;
      sse_send(c, frame, n);
      return;
    }
  }
  nc.stop();  // table full
}

void set_state_source(StateSource src) { g_source = src; }
void start() { g_srv.begin(); }

}  // namespace sse
