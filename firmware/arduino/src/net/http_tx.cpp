// See http_tx.h. Verbatim from the pre-split firmware.
#include "net/http_tx.h"

#include <Arduino.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

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
                          mime, static_cast<unsigned>(len), extra_hdr);
  if (hn < 0 || hn >= static_cast<int>(sizeof(hdr)))
    return;  // truncated header would corrupt the response framing
  if (send_bounded(s, hdr, hn))
    send_bounded(s, body, len);
  c.stop();  // Connection: close either way; a stalled peer is already gone
}

// ------------------------------------------------------------------ Chunked

Chunked::Chunked(WiFiClient c, const char* mime, const char* extra_hdr) : c_(c) {
  fd_ = c_.fd();
  if (fd_ < 0)
    return;
  char hdr[288];
  const int hn = snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                          "Transfer-Encoding: chunked\r\n%sConnection: close\r\n\r\n",
                          mime, extra_hdr);
  if (hn < 0 || hn >= static_cast<int>(sizeof(hdr)))
    return;
  ok_ = send_bounded(fd_, hdr, hn);
}

bool Chunked::flush() {
  if (!ok_ || len_ == 0)
    return ok_;
  // Chunk framing: hex length CRLF, payload, CRLF.
  char head[16];
  const int hn = snprintf(head, sizeof(head), "%x\r\n", static_cast<unsigned>(len_));
  if (hn < 0 || hn >= static_cast<int>(sizeof(head))) {
    ok_ = false;
    return false;
  }
  if (!send_bounded(fd_, head, hn) || !send_bounded(fd_, buf_, len_) ||
      !send_bounded(fd_, "\r\n", 2)) {
    ok_ = false;
    return false;
  }
  len_ = 0;
  return true;
}

bool Chunked::write(const char* p, size_t n) {
  while (ok_ && n > 0) {
    const size_t room = kBuf - len_;
    const size_t take = n < room ? n : room;
    memcpy(buf_ + len_, p, take);
    len_ += take;
    p += take;
    n -= take;
    if (len_ == kBuf && !flush())
      return false;
  }
  return ok_;
}

bool Chunked::print(const char* s) { return write(s, strlen(s)); }

bool Chunked::printf(const char* fmt, ...) {
  if (!ok_)
    return false;
  char tmp[160];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n < 0 || n >= static_cast<int>(sizeof(tmp))) {
    // Silent truncation here would emit malformed JSON under a 200 -- exactly
    // the failure this class exists to remove. Fail the stream instead.
    ok_ = false;
    return false;
  }
  return write(tmp, static_cast<size_t>(n));
}

void Chunked::end() {
  if (ended_)
    return;
  ended_ = true;
  if (ok_ && flush())
    send_bounded(fd_, "0\r\n\r\n", 5);
  c_.stop();
}

}  // namespace http_tx
