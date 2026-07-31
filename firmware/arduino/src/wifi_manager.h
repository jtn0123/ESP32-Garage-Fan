#pragma once

// WiFi management module - extracted from net.h
// Handles WiFi connection, provisioning, and status

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>

#if USE_WIFI_PROVISIONING
#include <esp_err.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <wifi_provisioning/manager.h>
#if WIFI_PROV_USE_SOFTAP
#include <wifi_provisioning/scheme_softap.h>
#endif
#if WIFI_PROV_USE_BLE
#include <wifi_provisioning/scheme_ble.h>
#endif
#endif

// WiFi connection parameters
#ifndef WIFI_CONNECT_TIMEOUT_MS
#define WIFI_CONNECT_TIMEOUT_MS 6000
#endif

#ifndef WIFI_RSSI_THRESHOLD
#define WIFI_RSSI_THRESHOLD -75
#endif

#ifndef WIFI_AUTHMODE_THRESHOLD
#define WIFI_AUTHMODE_THRESHOLD WIFI_AUTH_WPA_PSK
#endif

// WiFi connection state management
// WiFi management functions
bool wifi_connect_with_timeout(uint32_t timeout_ms);
bool wifi_connect_with_exponential_backoff(uint32_t max_attempts = 5,
                                           uint32_t initial_delay_ms = 1000);
bool wifi_is_connected();
String wifi_get_ip();
void wifi_get_ip_cstr(char* out, size_t out_size);
int wifi_get_rssi();
void wifi_begin_provisioning();
bool wifi_is_provisioning_active();

// BSSID utilities
bool parse_bssid(const char* str, uint8_t out[6]);
bool is_all_zero_bssid(const uint8_t b[6]);

void wifi_configure_power_save(bool enable);
// Drop TX power to 11 dBm when the link has margin to spare (RSSI > -65).
// Board heat management for the always-on build; no-op on a weak link.
void wifi_reduce_tx_power();

// Time sync functions
// - wifi_sync_time_ntp(): Syncs time via NTP when WiFi connected (automatic)
// - wifi_set_time_from_compile(): Uses compile timestamp as fallback when no WiFi
// - Time persists in RTC during deep sleep, only needs initial sync
void wifi_sync_time_ntp();
void wifi_set_time_from_compile();
