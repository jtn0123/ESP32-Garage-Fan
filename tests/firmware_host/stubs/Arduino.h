#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <utility>
class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  using std::string::operator=;
  explicit String(const std::string& s) : std::string(s) {}
  long toInt() const { return std::strtol(c_str(), nullptr, 10); }
  float toFloat() const { return std::strtof(c_str(), nullptr); }
};
struct SerialStub {
  template <class... T>
  void printf(const char*, T...) {}
  void println(const char*) {}
};
inline SerialStub Serial;
inline bool attach_ok = true, write_ok = true, restarted = false;
inline int wave_writes = 0;
inline bool ledcAttach(int, unsigned long, uint8_t) { return attach_ok; }
inline bool ledcWrite(int, uint32_t) {
  ++wave_writes;
  return write_ok;
}
inline void delay(unsigned long) {}
inline void esp_restart() { restarted = true; }
inline int digitalRead(int) { return 0; }
inline uint32_t micros() {
  static uint32_t now = 0;
  return now += 1000;
}
