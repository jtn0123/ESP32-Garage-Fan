#pragma once
#include "Arduino.h"
#include <cstring>
#include <map>
#include <string>
#include <vector>
class Preferences {
 public:
  bool fail_write = false;
  std::map<std::string, std::string> strings;
  std::map<std::string, double> numbers;
  std::map<std::string, std::vector<unsigned char>> blobs;
  String getString(const char* key, const char* fallback) {
    return String(strings.count(key) ? strings[key] : fallback);
  }
  size_t putString(const char* key, const String& value) {
    if (fail_write)
      return 0;
    strings[key] = value;
    return value.length();
  }
  size_t getBytesLength(const char* key) { return blobs[key].size(); }
  size_t getBytes(const char* key, void* out, size_t cap) {
    auto& b = blobs[key];
    if (b.size() > cap)
      return 0;
    std::memcpy(out, b.data(), b.size());
    return b.size();
  }
  size_t putBytes(const char* key, const void* input, size_t n) {
    if (fail_write)
      return 0;
    auto* p = static_cast<const unsigned char*>(input);
    blobs[key] = {p, p + n};
    return n;
  }
  int getInt(const char* key, int fallback) { return numbers.count(key) ? numbers[key] : fallback; }
  float getFloat(const char* key, float fallback) {
    return numbers.count(key) ? numbers[key] : fallback;
  }
  bool getBool(const char* key, bool fallback) {
    return numbers.count(key) ? numbers[key] : fallback;
  }
  uint16_t getUShort(const char* key, uint16_t fallback) {
    return numbers.count(key) ? numbers[key] : fallback;
  }
  size_t putInt(const char* key, int v) {
    if (fail_write)
      return 0;
    numbers[key] = v;
    return 4;
  }
  size_t putFloat(const char* key, float v) {
    if (fail_write)
      return 0;
    numbers[key] = v;
    return 4;
  }
  size_t putBool(const char* key, bool v) {
    if (fail_write)
      return 0;
    numbers[key] = v;
    return 1;
  }
  size_t putUShort(const char* key, uint16_t v) {
    if (fail_write)
      return 0;
    numbers[key] = v;
    return 2;
  }
};
