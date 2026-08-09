// See sse.h. Transport rules verbatim from the pre-split firmware.
#include "net/sse.h"

#include <Arduino.h>
#include <WiFi.h>

#include <cstdio>

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
  for (auto& c : g_peers) {
    if (!c || !c.connected()) {
      c = nc;
      c.setNoDelay(true);
      if (!g_source)
        return;
      char st[768];
      char frame[1024];
      g_source(st, sizeof(st));
      const int n = snprintf(frame, sizeof(frame),
                             "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                             "Cache-Control: no-cache\r\nAccess-Control-Allow-Origin: *\r\n"
                             "Connection: keep-alive\r\n\r\nretry: 3000\n\ndata: %s\n\n",
                             st);
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
