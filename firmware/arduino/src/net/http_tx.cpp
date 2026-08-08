// See http_tx.h. Verbatim from the pre-split firmware.
#include "net/http_tx.h"

#include <Arduino.h>

#include "esp_task_wdt.h"
#include "lwip/sockets.h"

namespace http_tx {

// Bounded raw send: WebServer's own writes go through NetworkClient::write,
// which burns up to 10 s of 1 s selects PER CALL against a peer that stops
// draining. The 21.8 KB page goes out in ~16 chunks, so one stalled browser
// meant 160 s of stall -- the 30 s task watchdog panicked mid-serve (the
// user-visible "clipped" truncated pages) and the board crash-looped. This
// path never blocks more than 50 ms at a time, feeds the watchdog between
// slices, and drops any peer that makes no progress for 4 s.
bool send_bounded(int s, const char* p, size_t n) {
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

void send_big(WiFiClient c, const char* mime, const char* body, size_t len, const char* extra_hdr) {
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

}  // namespace http_tx
