#pragma once
// Minimal Arduino.h for `platform = native` unit tests.
//
// Several modules are pure logic but include <Arduino.h> for types and a couple
// of timing calls. Rather than fork those modules or give up on testing them off
// target, this shim supplies just enough of the Arduino surface to compile them
// on the host.
//
// Deliberately NOT a general-purpose emulation:
//  - millis() is only *declared*. Each test defines it so it can control time.
//  - RTC_DATA_ATTR expands to nothing; on the host there is no RTC memory and no
//    deep sleep, so the variables are ordinary statics.
// Add to this file only what a test actually needs.

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// On the ESP32 this places a variable in RTC slow memory so it survives deep
// sleep. Host builds have neither, and the tests exercise the logic rather than
// the persistence, so it becomes a no-op.
#define RTC_DATA_ATTR

// Provided by each test so it can drive time deterministically.
uint32_t millis();

// No-op on the host: tests must not actually sleep.
inline void delay(uint32_t) {}
inline void delayMicroseconds(uint32_t) {}

// Stand-in for HardwareSerial so modules and tests that log can compile. Output
// goes to stdout, which is where the Unity test runner already writes.
class NativeSerial {
 public:
  void begin(unsigned long = 115200) {}
  void end() {}
  void flush() { fflush(stdout); }
  int available() { return 0; }

  size_t print(const char* s) {
    if (!s)
      return 0;
    fputs(s, stdout);
    return std::strlen(s);
  }
  size_t print(int v) { return printf("%d", v); }
  size_t print(unsigned v) { return printf("%u", v); }
  size_t print(float v) { return printf("%f", static_cast<double>(v)); }
  size_t println() { return printf("\n"); }
  size_t println(const char* s) { return printf("%s\n", s ? s : ""); }
  size_t println(int v) { return printf("%d\n", v); }

  int printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vprintf(fmt, args);
    va_end(args);
    return n;
  }
};

// C++17 inline variable: one shared instance without needing a .cpp in every
// test env that pulls this shim in.
inline NativeSerial Serial;

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef F
#define F(x) (x)
#endif

template <typename T>
constexpr const T& arduino_min(const T& a, const T& b) {
  return (b < a) ? b : a;
}
template <typename T>
constexpr const T& arduino_max(const T& a, const T& b) {
  return (a < b) ? b : a;
}

#ifndef min
#define min(a, b) arduino_min(a, b)
#endif
#ifndef max
#define max(a, b) arduino_max(a, b)
#endif

template <typename T>
constexpr T constrain(T value, T low, T high) {
  return (value < low) ? low : ((value > high) ? high : value);
}
