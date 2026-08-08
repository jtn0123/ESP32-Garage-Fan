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

}  // namespace http_tx
