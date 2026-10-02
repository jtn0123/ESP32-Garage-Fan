#pragma once
#include <cerrno>
#include <cmath>
#include <cctype>
#include <cstdlib>
namespace numeric_arg {
inline bool integer(const char* text, long low, long high, long* out) {
  if (!text || !*text || std::isspace(static_cast<unsigned char>(*text)))
    return false;
  char* end = nullptr;
  errno = 0;
  const long value = std::strtol(text, &end, 10);
  if (errno || end == text || *end || value < low || value > high)
    return false;
  *out = value;
  return true;
}
inline bool real(const char* text, float low, float high, float* out) {
  if (!text || !*text || std::isspace(static_cast<unsigned char>(*text)))
    return false;
  char* end = nullptr;
  errno = 0;
  const float value = std::strtof(text, &end);
  if (errno || end == text || *end || !std::isfinite(value) || value < low || value > high)
    return false;
  *out = value;
  return true;
}
}  // namespace numeric_arg
