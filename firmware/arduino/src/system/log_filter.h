#pragma once
// Which framework log lines are worth a slot on the flight recorder.
//
// The fuel-gauge probe emits four ESP_ERR_INVALID_STATE lines every ~30 s on
// the deployed board. They are benign -- battery::read has its own retry and
// the readings are fine -- but at that rate they were the majority of every
// /api/events tail, so the recorder built to explain a fault was mostly full
// of one that does not matter.
//
// Pure so the host can test it: the alternative is discovering a mis-tuned
// filter only when a real fault is missing from the tape.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace log_filter {

/** Repeating, benign I2C bus chatter from the gauge probe. */
inline bool is_i2c_noise(const char* s) {
  if (!s)
    return false;
  return strstr(s, "i2c_master_transmit_receive failed") != nullptr ||
         strstr(s, "i2cWriteReadNonStop returned Error") != nullptr;
}

/**
 * Keep the first `kKeep` of a boot, then suppress and count.
 *
 * The first occurrence is genuinely worth knowing; a permanent stream of them
 * is not. A periodic summary keeps the condition visible without letting it
 * evict real events.
 */
class NoiseGate {
 public:
  struct Verdict {
    bool emit;       // write this line to the tape
    bool summary;    // instead, write "<count> suppressed" now
    uint32_t count;  // how many were suppressed since the last summary
  };

  static constexpr uint8_t kKeep = 4;
  static constexpr uint32_t kSummaryMs = 3600000UL;  // hourly

  Verdict feed(bool noisy, uint32_t now_ms) {
    if (!noisy)
      return {true, false, 0};
    if (shown_ < kKeep) {
      shown_++;
      return {true, false, 0};
    }
    suppressed_++;
    if (now_ms - last_summary_ms_ > kSummaryMs) {
      last_summary_ms_ = now_ms;
      const uint32_t n = suppressed_;
      suppressed_ = 0;
      return {false, true, n};
    }
    return {false, false, 0};
  }

  uint32_t suppressed() const { return suppressed_; }

 private:
  uint8_t shown_ = 0;
  uint32_t suppressed_ = 0;
  uint32_t last_summary_ms_ = 0;
};

}  // namespace log_filter
