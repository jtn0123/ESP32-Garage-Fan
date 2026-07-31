#pragma once

#include <Arduino.h>  // For NAN

// Forward declarations for types used across multiple modules
// This helps avoid circular dependencies during refactoring

struct BatteryStatus;
struct MemoryDiagnostics;

// OutsideReadings moved here from net.h to avoid circular dependencies
struct OutsideReadings {
  float temperatureC = NAN;
  float humidityPct = NAN;
  char weather[64];
  float windMps = NAN;
  float highTempC = NAN;
  float lowTempC = NAN;
  float pressureHPa = NAN;
  bool validTemp = false;
  bool validHum = false;
  bool validWeather = false;
  bool validWind = false;
  bool validHigh = false;
  bool validLow = false;
  bool validPressure = false;
};

class Preferences;
class PubSubClient;
// No WiFiClient forward declaration: core 3.x makes it a typedef of
// NetworkClient, so `class WiFiClient;` is a compile error. The only
// consumer (mqtt_client.cpp) includes the real headers.

// Display-specific forward declarations
#if USE_DISPLAY
class GxEPD2_GFX_BASE_CLASS;
template <typename GxEPD2_Type, uint16_t page_height>
class GxEPD2_BW;
#endif

// Common type aliases
using DeviceId = char[40];
