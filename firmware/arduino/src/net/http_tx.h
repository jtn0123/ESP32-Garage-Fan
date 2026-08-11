#pragma once
// Bounded raw HTTP sends. WebServer's own writes go through
// NetworkClient::write, which burns up to 10 s of 1 s selects PER CALL
// against a peer that stops draining -- one stalled browser panicked the
// task watchdog mid-serve and crash-looped the board (proven live
// 2026-08-05). These paths never block more than 50 ms at a time, feed the
// watchdog between slices, and drop any peer that makes no progress for 4 s.

#include <WiFiClient.h>

#include <cstddef>

namespace http_tx {

/** Send exactly n bytes on fd s, or give up without ever stalling loop(). */
bool send_bounded(int s, const char* p, size_t n);

/** One-shot response on `c`: headers + body via send_bounded, then close. */
void send_big(WiFiClient c, const char* mime, const char* body, size_t len,
              const char* extra_hdr = "");

/**
 * Chunked response for bodies too big to hold in RAM at once.
 *
 * /api/history with per-row timestamps and all seven series runs past 11 KB
 * for a full 288-row window. It used to be accumulated into an Arduino
 * `String`, whose `reserve()` and `concat()` both return a bool that nothing
 * checked -- and this board's largest contiguous free block has been measured
 * at 7.7 KB on a busy boot. On failure the half-built string still went out
 * under a 200 and a matching Content-Length, so the console received
 * structurally invalid JSON (`..."hpa":[1013.2,1012`) and drew a blank chart
 * with no error recorded anywhere.
 *
 * Streaming removes the allocation entirely: at most `kBuf` bytes are ever
 * held, and a send failure is visible to the caller instead of being framed as
 * a successful truncated body.
 */
class Chunked {
 public:
  Chunked(WiFiClient c, const char* mime, const char* extra_hdr = "");
  Chunked(const Chunked&) = delete;
  Chunked& operator=(const Chunked&) = delete;

  /** Append to the body. False once the peer has gone; further calls no-op. */
  bool write(const char* p, size_t n);
  bool print(const char* s);
  /** printf into the buffer. Truncation is treated as a send failure. */
  bool printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

  /** Flush, send the terminating chunk and close. Safe to call twice. */
  void end();

  bool ok() const { return ok_; }

 private:
  bool flush();

  static constexpr size_t kBuf = 1024;
  WiFiClient c_;
  int fd_ = -1;
  bool ok_ = false;
  bool ended_ = false;
  size_t len_ = 0;
  char buf_[kBuf];
};

}  // namespace http_tx
