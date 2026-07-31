#pragma once

// Feature flags for conditional compilation
// Override in platformio.ini build_flags to disable features
//
// Example in platformio.ini:
//   build_flags = -D FEATURE_PROFILING=0 -D FEATURE_DEBUG_COMMANDS=0

// Home Assistant Discovery
#ifndef FEATURE_HA_DISCOVERY
#define FEATURE_HA_DISCOVERY 1
#endif

// Diagnostic mode support
#ifndef FEATURE_DIAGNOSTIC_MODE
#define FEATURE_DIAGNOSTIC_MODE 1
#endif

// Status LED/NeoPixel support
// Inherits from USE_STATUS_PIXEL if defined in config.h
#ifndef FEATURE_STATUS_PIXEL
#ifdef USE_STATUS_PIXEL
#define FEATURE_STATUS_PIXEL USE_STATUS_PIXEL
#else
#define FEATURE_STATUS_PIXEL 0
#endif
#endif

// Pressure sensor (BME280/BMP280)
#ifndef FEATURE_PRESSURE_SENSOR
#define FEATURE_PRESSURE_SENSOR 1
#endif

// Outdoor weather data via MQTT
#ifndef FEATURE_OUTDOOR_WEATHER
#define FEATURE_OUTDOOR_WEATHER 1
#endif

// Battery monitoring (MAX17048/LC709203F)
#ifndef FEATURE_BATTERY_MONITOR
#define FEATURE_BATTERY_MONITOR 1
#endif

// Debug commands via MQTT
#ifndef FEATURE_DEBUG_COMMANDS
#define FEATURE_DEBUG_COMMANDS 1
#endif

// Performance profiling (enabled in debug builds by default)
#ifndef FEATURE_PROFILING
#ifdef DEBUG
#define FEATURE_PROFILING 1
#else
#define FEATURE_PROFILING 0
#endif
#endif

// Memory tracking
#ifndef FEATURE_MEMORY_TRACKING
#define FEATURE_MEMORY_TRACKING 1
#endif

// Crash handler
#ifndef FEATURE_CRASH_HANDLER
#define FEATURE_CRASH_HANDLER 1
#endif

// microSD storage: sensor history CSV, rotating log files, config overrides and
// staged firmware images.
#ifndef FEATURE_SD_STORAGE
#define FEATURE_SD_STORAGE 1
#endif

// USB mass-storage: history/logs mount as a read-only flash drive on a host.
// Requires the tinyusb (OTG) USB stack; only meaningful on always-on nodes.
#ifndef FEATURE_USB_MSC
#define FEATURE_USB_MSC 0
#endif

// Over-the-air firmware updates (network via ArduinoOTA, plus apply-from-SD).
#ifndef FEATURE_OTA
#define FEATURE_OTA 1
#endif

// Helper macros
#define FEATURE_ENABLED(x) (FEATURE_##x == 1)
#define FEATURE_DISABLED(x) (FEATURE_##x == 0)

// Compile-time assertions for incompatible combinations
#if FEATURE_HA_DISCOVERY && !FEATURE_OUTDOOR_WEATHER
#warning "HA Discovery without outdoor weather may have limited functionality"
#endif
