// WiFi management implementation - extracted from net.h
#include "wifi_manager.h"
#include <sys/time.h>
#include <time.h>
#include <algorithm>
#include <cstdio>
#include "generated_config.h"
#include "config.h"
#include "profiling.h"
#include "runtime_config.h"

// Static storage for provisioning
static Preferences g_wifi_prefs;

// Track if time has been synced (persists in RTC during deep sleep)
RTC_DATA_ATTR static bool g_time_synced = false;
RTC_DATA_ATTR static uint32_t g_last_ntp_sync_timestamp = 0;

bool parse_bssid(const char* str, uint8_t out[6]) {
  if (!str)
    return false;
  int vals[6];
  int n =
      sscanf(str, "%x:%x:%x:%x:%x:%x", &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]);
  if (n != 6)
    return false;
  for (int i = 0; i < 6; ++i) out[i] = static_cast<uint8_t>(vals[i]);
  return true;
}

bool is_all_zero_bssid(const uint8_t b[6]) {
  for (int i = 0; i < 6; ++i)
    if (b[i] != 0)
      return false;
  return true;
}

bool wifi_connect_with_timeout(uint32_t timeout_ms) {
  // Try to connect with configured credentials
  WiFi.mode(WIFI_STA);

  // Parse BSSID if configured
  uint8_t bssid_bytes[6] = {0};
  bool has_bssid = false;

#ifdef WIFI_BSSID
  // WIFI_BSSID pins the connection to one specific access point of the network
  // that was compiled in. If the SD card has since pointed us at a *different*
  // SSID, that BSSID belongs to the old network and forcing it guarantees the
  // override cannot connect.
  if (rc_wifi_overridden()) {
    Serial.println("[WiFi] SSID overridden by config; ignoring compiled-in BSSID");
  } else {
    has_bssid = parse_bssid(WIFI_BSSID, bssid_bytes);
    if (!has_bssid || is_all_zero_bssid(bssid_bytes)) {
      has_bssid = false;
    }
  }
#endif

  // Credentials come from the runtime config, so they can be supplied either at
  // build time or by /config/device.json on the SD card.
  const char* ssid = rc_wifi_ssid();
  const char* pass = rc_wifi_pass();

  // Set hostname before connecting
  WiFi.setHostname(rc_room_name());

  // Connect with or without BSSID
  if (has_bssid) {
    Serial.printf("[WiFi] Connecting to %s with BSSID\n", ssid);
    WiFi.begin(ssid, pass, 0, bssid_bytes);
  } else {
    Serial.printf("[WiFi] Connecting to %s\n", ssid);
    WiFi.begin(ssid, pass);
  }

  // Wait for connection with timeout
  uint32_t start = millis();
  while (!WiFi.isConnected() && (millis() - start < timeout_ms)) {
    delay(100);
  }

  if (WiFi.isConnected()) {
    Serial.printf("[WiFi] Connected! IP: %s, RSSI: %d\n", WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());

    // Sync time via NTP (only if not already synced this session)
    wifi_sync_time_ntp();

    return true;
  } else {
    Serial.printf("[WiFi] Connection failed after %dms\n", timeout_ms);
    return false;
  }
}

bool wifi_connect_with_exponential_backoff(uint32_t max_attempts, uint32_t initial_delay_ms) {
  PROFILE_SCOPE("wifi_connect_backoff");
  uint32_t retry_delay_ms = initial_delay_ms;

  for (uint32_t attempt = 0; attempt < max_attempts; attempt++) {
    Serial.printf("[WiFi] Connection attempt %d/%d\n", attempt + 1, max_attempts);

    if (wifi_connect_with_timeout(WIFI_CONNECT_TIMEOUT_MS)) {
      return true;
    }

    // Don't delay after the last attempt
    if (attempt < max_attempts - 1) {
      Serial.printf("[WiFi] Waiting %dms before retry...\n", retry_delay_ms);
      delay(retry_delay_ms);

      // Exponential backoff with cap at 16 seconds
      retry_delay_ms = min(retry_delay_ms * 2, static_cast<uint32_t>(16000));
    }
  }

  Serial.printf("[WiFi] Failed to connect after %d attempts\n", max_attempts);
  return false;
}

bool wifi_is_connected() { return WiFi.isConnected(); }

String wifi_get_ip() {
  if (!WiFi.isConnected()) {
    return String("0.0.0.0");
  }
  return WiFi.localIP().toString();
}

void wifi_get_ip_cstr(char* out, size_t out_size) {
  if (!out || out_size == 0)
    return;

  if (!WiFi.isConnected()) {
    snprintf(out, out_size, "0.0.0.0");
    return;
  }

  IPAddress ip = WiFi.localIP();
  snprintf(out, out_size, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

int wifi_get_rssi() {
  if (!WiFi.isConnected()) {
    return -100;
  }
  return WiFi.RSSI();
}

void wifi_configure_power_save(bool enable) {
  if (enable) {
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
  } else {
    WiFi.setSleep(WIFI_PS_NONE);
  }
}

void wifi_reduce_tx_power() {
  // Default TX power is 19.5 dBm, sized for reaching an AP across a house. This
  // node sits at RSSI -53; 11 dBm still leaves ~20 dB of link margin, and every
  // transmit at lower power is heat the on-board BME280 never sees. Skipped
  // when the link is already marginal — a dropped connection costs far more
  // (reconnect bursts at full power) than the savings.
  if (WiFi.isConnected() && WiFi.RSSI() > -65) {
    WiFi.setTxPower(WIFI_POWER_11dBm);
  }
}

#if USE_WIFI_PROVISIONING
void wifi_begin_provisioning() {
  // Implementation would go here - complex provisioning logic
  // For now, just a stub
}

bool wifi_is_provisioning_active() {
  // Check if provisioning manager is active
  return false;  // Stub for now
}
#else
void wifi_begin_provisioning() {
  // No-op without provisioning support
}

bool wifi_is_provisioning_active() { return false; }
#endif

// ============================================================================
// Time Sync Functions
// ============================================================================

// Parse compile-time __DATE__ and __TIME__ into a tm struct
// __DATE__ format: "Dec  4 2025"
// __TIME__ format: "10:32:15"
static bool parse_compile_time(struct tm* tm_out) {
  static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

  char month_str[4];
  int day, year, hour, minute, second;

  // Parse __DATE__
  if (sscanf(__DATE__, "%3s %d %d", month_str, &day, &year) != 3) {
    return false;
  }

  // Parse __TIME__
  if (sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) {
    return false;
  }

  // Find month index
  int month = -1;
  for (int i = 0; i < 12; i++) {
    if (strcmp(month_str, months[i]) == 0) {
      month = i;
      break;
    }
  }
  if (month < 0)
    return false;

  // Fill tm struct
  tm_out->tm_year = year - 1900;  // Years since 1900
  tm_out->tm_mon = month;         // 0-11
  tm_out->tm_mday = day;
  tm_out->tm_hour = hour;
  tm_out->tm_min = minute;
  tm_out->tm_sec = second;
  tm_out->tm_isdst = -1;  // Let system determine DST

  return true;
}

void wifi_set_time_from_compile() {
  // Only set if time is clearly invalid (before 2024)
  time_t now = time(nullptr);
  struct tm tm_check;
  localtime_r(&now, &tm_check);

  if (tm_check.tm_year >= 124) {  // 2024 or later - time already set
    Serial.println("[Time] RTC already has valid time, skipping compile-time fallback");
    return;
  }

  // Parse compile timestamp
  struct tm compile_tm = {};
  if (!parse_compile_time(&compile_tm)) {
    Serial.println("[Time] Failed to parse compile time");
    return;
  }

  // Convert to time_t and set system time
  time_t compile_time = mktime(&compile_tm);
  if (compile_time == (time_t)-1) {
    Serial.println("[Time] Failed to convert compile time");
    return;
  }

  struct timeval tv = {.tv_sec = compile_time, .tv_usec = 0};
  settimeofday(&tv, nullptr);

  Serial.printf("[Time] Set to compile time: %s", asctime(&compile_tm));
}

void wifi_sync_time_ntp() {
  // Skip if already synced this boot cycle and time looks valid
  time_t now = time(nullptr);
  struct tm tm_check;
  localtime_r(&now, &tm_check);

  if (g_time_synced && tm_check.tm_year >= 124) {
    Serial.println("[Time] Already synced via NTP this session");
    return;
  }

  Serial.println("[Time] Syncing via NTP...");

  // Configure timezone (EST/EDT - adjust for your location)
  // Format: "STD+offset" or "STD+offset DST"
  // EST5EDT = Eastern Standard Time, 5 hours behind UTC, with DST
  configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov", "time.google.com");

  // Wait for time sync (max 5 seconds to not delay boot too much)
  uint32_t start = millis();
  const uint32_t NTP_TIMEOUT_MS = 5000;

  while (time(nullptr) < 1700000000 && (millis() - start) < NTP_TIMEOUT_MS) {
    delay(100);
  }

  now = time(nullptr);
  if (now > 1700000000) {  // After Nov 2023 = valid
    g_time_synced = true;
    g_last_ntp_sync_timestamp = static_cast<uint32_t>(now);

    struct tm tm_now;
    localtime_r(&now, &tm_now);
    Serial.printf("[Time] NTP sync successful: %02d:%02d:%02d\n", tm_now.tm_hour, tm_now.tm_min,
                  tm_now.tm_sec);
  } else {
    Serial.println("[Time] NTP sync failed, using existing time");
  }
}
