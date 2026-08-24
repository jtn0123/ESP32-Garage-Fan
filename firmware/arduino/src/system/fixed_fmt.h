#pragma once
// Copyright 2026 Justin
//
// Rendering a measurement into a log line without letting an absurd value
// eat the line.
//
// The flight recorder renders at most 80 bytes per message and truncates the
// TAIL -- the end of a telemetry line is where the counts live, so a line
// that overruns loses exactly the fields worth reading, silently. "%.1f" of
// an arbitrary float can be over three hundred characters (1e38 is 39 digits
// before the point), which is not hypothetical here: the CSV reader hands
// back whatever a corrupt row held, and a wedged I2C transaction has already
// produced -162.9 hPa on this device.
//
// So numbers destined for a log line go through here: clamped to tenths in a
// range that fits, formatted from INTEGERS. The compiler can follow integer
// ranges (it cannot follow float ones), which turns "this line fits" from a
// hope into something -Wformat-truncation checks at every build.
//
// Pure header; native_fixed_fmt exercises the rounding, the clamp and the
// sign handling that a naive t/10 gets wrong for -0.5.

#include <stdint.h>

#include <cmath>
#include <cstdio>

namespace fixedfmt {

/** Widest buffer any rendering here needs: "-999.9" plus the terminator. */
inline constexpr size_t kCap = 8;

/** No reading. Distinct from every value tenths() can return. */
inline constexpr int32_t kAbsent = INT32_MIN;

/** `v` in tenths, rounded, clamped to +/-999.9; NaN becomes kAbsent. */
inline int32_t tenths(float v) {
  if (std::isnan(v))
    return kAbsent;
  // Two clamps, and both are load-bearing.
  //
  // The FLOAT one first, because lround() of an infinity is undefined and the
  // platforms disagree about it: v * 10 overflows to +inf for anything past
  // ~3.4e37, glibc's lround then returns LONG_MIN, and "the largest possible
  // reading" would render as -999.9. (Written as !(x < limit) so it also
  // catches a NaN produced by the multiply itself.)
  const float t = v * 10.0f;
  if (!(t < 9999.5f))
    return 9999;
  if (!(t > -9999.5f))
    return -9999;
  // Then the INTEGER one, because the compiler tracks integer ranges and does
  // not track float ones -- this is what lets -Wformat-truncation prove the
  // rendered field is at most six characters.
  long r = std::lround(t);
  if (r > 9999)
    r = 9999;
  if (r < -9999)
    r = -9999;
  return static_cast<int32_t>(r);
}

/** Celsius in tenths of Fahrenheit, same clamp and sentinel. */
inline int32_t tenths_f(float c) { return std::isnan(c) ? kAbsent : tenths(c * 9 / 5 + 32); }

/**
 * "-99.9" / "0.4" / "--" into a buffer the compiler knows the size of.
 *
 * The sign is handled before the split because -5 tenths is -0.5, and
 * "%ld.%ld" of (-5/10, -5%10) renders "0.-5".
 */
inline void write(char (&out)[kCap], int32_t t) {  // NOLINT(runtime/references)
  if (t == kAbsent) {
    out[0] = '-';
    out[1] = '-';
    out[2] = '\0';
    return;
  }
  // Re-clamp rather than trust the caller: this is what lets the compiler
  // prove a/10 is at most three digits, which is what keeps -Wformat-truncation
  // checking every log line that goes through here.
  if (t > 9999)
    t = 9999;
  if (t < -9999)
    t = -9999;
  const int32_t a = t < 0 ? -t : t;  // 0..9999, so a/10 is at most 3 digits
  snprintf(out, kCap, "%s%ld.%ld", t < 0 ? "-" : "", static_cast<long>(a / 10),
           static_cast<long>(a % 10));
}

/** Same, with an explicit sign on positives ("+9.4") for differentials. */
inline void write_signed(char (&out)[kCap], int32_t t) {  // NOLINT(runtime/references)
  if (t == kAbsent || t < 0) {
    write(out, t);
    return;
  }
  const int32_t a = t > 9999 ? 9999 : t;
  snprintf(out, kCap, "+%ld.%ld", static_cast<long>(a / 10), static_cast<long>(a % 10));
}

/** A count in at most two digits, so a runaway counter costs its own field. */
inline int count99(uint16_t v) { return v > 99 ? 99 : static_cast<int>(v); }

/** A count in at most three digits. */
inline int count999(uint16_t v) { return v > 999 ? 999 : static_cast<int>(v); }

/** A speed-like number in at most three characters, sign included. */
inline int small(int v) { return v > 99 ? 99 : (v < -9 ? -9 : v); }

}  // namespace fixedfmt
