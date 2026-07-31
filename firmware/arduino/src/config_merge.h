#pragma once
// Copyright 2024 Justin
//
// Pure merge helpers for overlaying an external config source (the JSON file on
// the microSD card) onto the values compiled into the firmware.
//
// The contract these helpers implement is the reason a bad card cannot brick the
// device: an override only wins when it is actually present and sane. Anything
// missing, blank, or out of range leaves the compiled-in default in place.
//
// Deliberately free of Arduino/ESP dependencies so the semantics can be unit
// tested on the native platform.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Copy `src` over `dst` only when `src` is a non-empty string. Always leaves
// `dst` NUL-terminated. Returns true when the override was applied.
inline bool merge_str(char* dst, size_t dst_size, const char* src) {
  if (!dst || dst_size == 0)
    return false;
  if (!src || src[0] == '\0')
    return false;

  // snprintf rather than safe_strcpy_rt(): this header is deliberately free of
  // Arduino/ESP dependencies so the native unit tests can include it directly,
  // and safe_strings.h pulls in logging.h -> Arduino.h. snprintf truncates and
  // NUL-terminates, which is the same guarantee, and strncpy is banned repo-wide
  // precisely because it does not.
  std::snprintf(dst, dst_size, "%s", src);
  return true;
}

// Apply `src` over `dst` only when it falls within [min_val, max_val].
// Out-of-range values are rejected rather than clamped: a card that says "0" or
// "999999" is more likely a mistake than an intent, and silently clamping would
// hide it.
inline bool merge_u32(uint32_t* dst, int64_t src, int64_t min_val, int64_t max_val) {
  if (!dst)
    return false;
  if (src < min_val || src > max_val)
    return false;

  *dst = static_cast<uint32_t>(src);
  return true;
}

inline bool merge_u16(uint16_t* dst, int64_t src, int64_t min_val, int64_t max_val) {
  if (!dst)
    return false;
  if (src < min_val || src > max_val)
    return false;

  *dst = static_cast<uint16_t>(src);
  return true;
}

// Booleans have no "absent" representation of their own, so presence is decided
// by the caller (which can see whether the JSON key existed) and passed in.
inline bool merge_bool(bool* dst, bool present, bool src) {
  if (!dst || !present)
    return false;

  *dst = src;
  return true;
}

// Floats can be legitimately negative or zero, so like merge_bool the caller
// decides presence; unlike the integer helpers there is no sentinel that could
// stand in for "absent". Out-of-range values are rejected rather than clamped.
inline bool merge_float(float* dst, bool present, double src, double min_val, double max_val) {
  if (!dst || !present)
    return false;
  if (src < min_val || src > max_val)
    return false;

  *dst = static_cast<float>(src);
  return true;
}
