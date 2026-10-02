#pragma once
#include "Arduino.h"
struct IpStub {
  String toString() { return "127.0.0.1"; }
};
struct WifiStub {
  IpStub localIP() { return {}; }
};
inline WifiStub WiFi;
