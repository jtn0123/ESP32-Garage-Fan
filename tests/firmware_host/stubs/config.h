#pragma once
#include <cstdint>
constexpr int FAN_PWM_PIN = 18;
constexpr uint16_t kPeriodUs = 9934;
constexpr uint16_t kHighUs[13] = {0,    450,  750,  1000, 1500, 2000, 2500,
                                  3000, 4000, 5000, 6000, 7500, 9934};
constexpr unsigned kAutoTickMs = 30000;
#define WIFI_SSID "old-net"
#define WIFI_PASS "old-pass"
#define MQTT_HOST "old-host"
#define MQTT_PORT 1883
#define MQTT_USER "old-user"
#define MQTT_PASS "old-mqtt-pass"
#define WEATHER_LAT ""
#define WEATHER_LON ""
#define FAN_HOSTNAME "garage-fan"
