#pragma once
// Block-buffered line splitting for the CSV readers.
//
// Templated on the byte source so the logic is testable off-device: a source
// needs only
//     int read(uint8_t* dst, size_t cap)   // <=0 means end of input
// which fs::File already satisfies. kBlock is a template parameter for the
// same reason -- the tests shrink it to a handful of bytes so a line that
// straddles a refill boundary is cheap to reach.
//
// Why this exists: the reader it replaced called File::read() once per
// CHARACTER, and each of those crosses Arduino File -> VFS -> FATFS -> SPI.
// Across two passes over a month of samples that per-character overhead is
// essentially the whole cost of a chart request -- and it is paid with the
// loop blocked, which is the exact failure mode that spent 2026-08-09
// disguised as a random MQTT dropout.

#include <stddef.h>
#include <stdint.h>

namespace storage {

template <class Source, size_t kBlock = 512>
class LineReader {
 public:
  explicit LineReader(Source& src) : src_(src) {}

  /**
   * Next line into `out`: newline stripped, CR dropped, always terminated.
   * False once the source is exhausted.
   *
   * A trailing line with no newline is returned rather than dropped. The
   * character-at-a-time loop this replaced only emitted a line when it saw
   * '\n', so it silently discarded the last row of any file that did not end
   * in one -- invisible in a chart, and exactly the sort of quiet data loss
   * the reader should not be deciding on its own.
   *
   * Lines longer than `cap` are truncated, not split: the caller's parse of a
   * truncated row fails cleanly, whereas a split would manufacture a second
   * row out of the tail.
   */
  bool next(char* out, size_t cap) {
    if (!out || cap == 0)
      return false;
    size_t n = 0;
    bool consumed = false;
    for (;;) {
      if (pos_ >= len_) {
        const int got = static_cast<int>(src_.read(buf_, kBlock));
        if (got <= 0)
          break;
        len_ = static_cast<size_t>(got) > kBlock ? kBlock : static_cast<size_t>(got);
        pos_ = 0;
      }
      consumed = true;
      const char c = static_cast<char>(buf_[pos_++]);
      if (c == '\n') {
        out[n] = '\0';
        return true;
      }
      if (c != '\r' && n + 1 < cap)
        out[n++] = c;
    }
    out[n] = '\0';
    return consumed && n > 0;
  }

 private:
  Source& src_;
  uint8_t buf_[kBlock];
  size_t len_ = 0;
  size_t pos_ = 0;
};

}  // namespace storage
