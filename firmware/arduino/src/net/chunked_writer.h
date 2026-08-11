#pragma once
// HTTP/1.1 chunked-transfer framing, templated on the byte sink so the exact
// wire bytes can be asserted off-device.
//
// A sink needs only
//     bool send(const char* p, size_t n)   // false = peer is gone
//
// Why this is worth testing: every response too large to hold in RAM goes
// through here -- /api/history (past 11 KB for a full window) and the whole
// /download.csv export. Chunk framing is length-prefixed hex, so a single
// wrong length or a missing CRLF does not degrade gracefully; the client
// rejects or hangs on the entire body. The String-accumulating version this
// replaced failed exactly that way, emitting truncated JSON under a 200 and a
// matching Content-Length, and nothing noticed because nothing checked.

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace net {

template <class Sink, size_t kBuf = 1024>
class ChunkedWriter {
 public:
  explicit ChunkedWriter(Sink& sink) : sink_(sink) {}

  /** Send the response head. False if the peer is already gone. */
  bool begin(const char* mime, const char* extra_hdr = "") {
    char hdr[288];
    const int n = snprintf(hdr, sizeof(hdr),
                           "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                           "Transfer-Encoding: chunked\r\n%sConnection: close\r\n\r\n",
                           mime, extra_hdr);
    if (n < 0 || n >= static_cast<int>(sizeof(hdr)))
      return false;  // a truncated header would corrupt the framing
    ok_ = sink_.send(hdr, static_cast<size_t>(n));
    return ok_;
  }

  /** Append to the body. Once false, every later call is a no-op. */
  bool write(const char* p, size_t n) {
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

  bool print(const char* s) { return s ? write(s, strlen(s)) : ok_; }

  /**
   * printf into the body. Truncation FAILS the stream rather than emitting a
   * short field: a clipped number is malformed JSON, and the point of this
   * class is that a body is either correct or visibly broken.
   */
  bool printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list ap;
    va_start(ap, fmt);
    const bool r = vprintf(fmt, ap);
    va_end(ap);
    return r;
  }

  /** printf's body, so a caller holding a va_list can forward to it. */
  bool vprintf(const char* fmt, va_list ap) {
    if (!ok_)
      return false;
    char tmp[160];
    const int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    if (n < 0 || n >= static_cast<int>(sizeof(tmp))) {
      ok_ = false;
      return false;
    }
    return write(tmp, static_cast<size_t>(n));
  }

  /** Flush and send the terminating zero-length chunk. Idempotent. */
  void end() {
    if (ended_)
      return;
    ended_ = true;
    if (ok_ && flush())
      sink_.send("0\r\n\r\n", 5);
  }

  bool ok() const { return ok_; }
  bool ended() const { return ended_; }

 private:
  bool flush() {
    if (!ok_ || len_ == 0)
      return ok_;
    char head[16];
    const int n = snprintf(head, sizeof(head), "%x\r\n", static_cast<unsigned>(len_));
    if (n < 0 || n >= static_cast<int>(sizeof(head))) {
      ok_ = false;
      return false;
    }
    if (!sink_.send(head, static_cast<size_t>(n)) || !sink_.send(buf_, len_) ||
        !sink_.send("\r\n", 2)) {
      ok_ = false;
      return false;
    }
    len_ = 0;
    return true;
  }

  Sink& sink_;
  bool ok_ = false;
  bool ended_ = false;
  size_t len_ = 0;
  char buf_[kBuf];
};

}  // namespace net
