#pragma once
// Minimal esp_system.h for `platform = native` unit tests.
//
// Only the reset-reason enum is needed: modules take it as a parameter and map it
// to a string, which is exactly the kind of pure logic worth testing on the host.
// Values match ESP-IDF's ordering so a test asserting on a specific enumerator
// stays meaningful.

typedef enum {
  ESP_RST_UNKNOWN = 0,
  ESP_RST_POWERON = 1,
  ESP_RST_EXT = 2,
  ESP_RST_SW = 3,
  ESP_RST_PANIC = 4,
  ESP_RST_INT_WDT = 5,
  ESP_RST_TASK_WDT = 6,
  ESP_RST_WDT = 7,
  ESP_RST_DEEPSLEEP = 8,
  ESP_RST_BROWNOUT = 9,
  ESP_RST_SDIO = 10,
} esp_reset_reason_t;

typedef enum {
  ESP_SLEEP_WAKEUP_UNDEFINED = 0,
  ESP_SLEEP_WAKEUP_ALL = 1,
  ESP_SLEEP_WAKEUP_EXT0 = 2,
  ESP_SLEEP_WAKEUP_EXT1 = 3,
  ESP_SLEEP_WAKEUP_TIMER = 4,
  ESP_SLEEP_WAKEUP_TOUCHPAD = 5,
  ESP_SLEEP_WAKEUP_ULP = 6,
  ESP_SLEEP_WAKEUP_GPIO = 7,
  ESP_SLEEP_WAKEUP_UART = 8,
} esp_sleep_wakeup_cause_t;
