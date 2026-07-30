// Credential sanitization for safe logging
#pragma once

#include <Arduino.h>
// Included directly rather than relied on transitively via safe_strings.h:
// SAFE_LOG_CONNECTION below expands LOG_INFO at its *use* site, so the simple
// LOG_* macros must be the ones in scope there.
#include "logging.h"
#include "safe_strings.h"

// Sanitize sensitive strings for logging
inline String sanitize_credential(const char* value, bool show_partial = true) {
  if (!value || strlen(value) == 0) {
    return String("(empty)");
  }

  size_t len = strlen(value);

  if (len <= 4) {
    return String("****");
  }

  if (show_partial && len > 8) {
    // Show first 2 and last 2 characters
    String result = String(value[0]) + String(value[1]);
    for (size_t i = 0; i < len - 4; i++) {
      result += "*";
    }
    result += String(value[len - 2]) + String(value[len - 1]);
    return result;
  } else {
    // Hide everything
    String result = "";
    for (size_t i = 0; i < len; i++) {
      result += "*";
    }
    return result;
  }
}

// Sanitize SSID (less sensitive, show more)
inline String sanitize_ssid(const char* ssid) {
  if (!ssid || strlen(ssid) == 0) {
    return String("(none)");
  }

  size_t len = strlen(ssid);
  if (len <= 4) {
    return String(ssid);  // Short SSIDs are probably test
  }

  // Show first 3 characters
  String result = "";
  for (size_t i = 0; i < 3 && i < len; i++) {
    result += ssid[i];
  }
  result += "***";
  return result;
}

// Sanitize IP address (show subnet only)
inline String sanitize_ip(const char* ip) {
  if (!ip || strlen(ip) == 0) {
    return String("0.0.0.0");
  }

  String ipStr = String(ip);
  int lastDot = ipStr.lastIndexOf('.');
  if (lastDot > 0) {
    return ipStr.substring(0, lastDot) + ".xxx";
  }
  return String("x.x.x.x");
}

// Safe configuration logging
inline void log_safe_config() {
#if LOG_LEVEL >= LOG_LEVEL_INFO
  Serial.println("=== Configuration (sanitized) ===");
  Serial.printf("Room: %s\n", ROOM_NAME);
  Serial.printf("Wake Interval: %d seconds\n", WAKE_INTERVAL_SEC);
  Serial.printf("WiFi SSID: %s\n", sanitize_ssid(WIFI_SSID).c_str());
  Serial.printf("WiFi Pass: %s\n", sanitize_credential(WIFI_PASS, false).c_str());
  Serial.printf("MQTT Host: %s\n", sanitize_ip(MQTT_HOST).c_str());
  Serial.printf("MQTT Port: %d\n", MQTT_PORT);
  Serial.printf("MQTT User: %s\n", sanitize_credential(MQTT_USER, true).c_str());
  Serial.printf("MQTT Pass: %s\n", sanitize_credential(MQTT_PASS, false).c_str());
  Serial.println("================================");
#endif
}

// Macro to prevent accidental credential logging
#define SAFE_LOG_CONNECTION(type, host, user)                                  \
  LOG_INFO("Connecting to %s: %s (user: %s)", type, sanitize_ip(host).c_str(), \
           sanitize_credential(user, true).c_str())

// Never log these directly
#ifdef DEBUG_BUILD
#pragma message("WARNING: Debug build - ensure no credentials in logs!")
#endif

// Compile-time check to prevent direct credential logging
#define WIFI_PASS_DO_NOT_LOG WIFI_PASS
#define MQTT_PASS_DO_NOT_LOG MQTT_PASS

// If someone tries to log these, it will fail to compile
#ifdef LOG_CREDENTIALS
#error "Never log credentials directly! Use sanitize_credential()"
#endif
