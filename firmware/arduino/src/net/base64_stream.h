#pragma once
// Streaming base64, for putting a framebuffer inside a JSON body.
//
// Streaming rather than encode-to-buffer because the thing being encoded is two
// 3904-byte display planes: buffering the result would mean a ~10 KB allocation
// on a board whose largest contiguous free block has been measured at 7.7 KB on
// a busy boot. That is the exact failure http_tx::Chunked was written to remove,
// and re-introducing it one layer up would undo it.
//
// Pure and header-only so the host can test it against a known-good encoder;
// the padding rules at the 1- and 2-byte tails are the part that gets written
// wrong, and they only show up on inputs whose length is not a multiple of 3.

#include <stddef.h>
#include <stdint.h>

namespace base64_stream {

inline constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * Encode `n` bytes, handing complete 4-character groups to `sink`.
 *
 * `sink` returns false to abort (a dead peer); encoding stops there and this
 * returns false. Call flush() once at the end for the final partial group.
 */
class Encoder {
 public:
  /** Feed bytes. Returns false once the sink has refused. */
  template <typename Sink>
  bool write(const uint8_t* data, size_t n, Sink&& sink) {
    if (!ok_)
      return false;
    for (size_t i = 0; i < n; i++) {
      acc_ = (acc_ << 8) | data[i];
      bits_ += 8;
      while (bits_ >= 6) {
        bits_ -= 6;
        char c = kAlphabet[(acc_ >> bits_) & 0x3F];
        if (!sink(&c, 1)) {
          ok_ = false;
          return false;
        }
      }
    }
    return true;
  }

  /**
   * Emit the tail and its padding.
   *
   * 1 leftover byte -> two characters and "==", 2 -> three and "=". Getting
   * this wrong produces a body that decodes for every input whose length
   * happens to divide by 3 and silently corrupts the rest.
   */
  template <typename Sink>
  bool flush(Sink&& sink) {
    if (!ok_)
      return false;
    if (bits_ > 0) {
      char c = kAlphabet[(acc_ << (6 - bits_)) & 0x3F];
      if (!sink(&c, 1)) {
        ok_ = false;
        return false;
      }
      bits_ = 0;
    }
    while (out_pad_() > 0) {
      char eq = '=';
      if (!sink(&eq, 1)) {
        ok_ = false;
        return false;
      }
      pad_++;
    }
    done_ = true;
    return true;
  }

  bool ok() const { return ok_; }

  /** How many characters a body of `n` bytes encodes to, padding included. */
  static constexpr size_t encoded_len(size_t n) { return ((n + 2) / 3) * 4; }

 private:
  /** Padding still owed: enough to round the emitted count up to a multiple of 4. */
  int out_pad_() const {
    if (done_)
      return 0;
    const int rem = static_cast<int>(total_ % 3);
    const int need = rem == 0 ? 0 : (rem == 1 ? 2 : 1);
    return need - pad_;
  }

 public:
  /** Bytes fed so far; the padding rule is derived from this. */
  void count(size_t n) { total_ += n; }

 private:
  uint32_t acc_ = 0;
  int bits_ = 0;
  size_t total_ = 0;
  int pad_ = 0;
  bool ok_ = true;
  bool done_ = false;
};

/**
 * One-shot convenience: encode a whole buffer through `sink`.
 *
 * The common case here, since a display plane is contiguous and already in RAM.
 */
template <typename Sink>
inline bool encode(const uint8_t* data, size_t n, Sink&& sink) {
  Encoder e;
  e.count(n);
  if (!e.write(data, n, sink))
    return false;
  return e.flush(sink);
}

}  // namespace base64_stream
