// State management implementation
#include "state_manager.h"
// std::isfinite, not bare isfinite(): <cmath> only guarantees the std-qualified
// name. Clang/libc++ happens to inject the unqualified one too, so the bare call
// built fine on the ESP32 toolchain and on macOS, but GCC rejects it -- which is
// what the native unit tests build with on Linux CI.
#include <cmath>

// RTC_DATA_ATTR variables persist across deep sleep but not power cycles
// Display state
RTC_DATA_ATTR uint16_t partial_counter = 0;

// Temperature and sensor state
RTC_DATA_ATTR static float last_inside_f = NAN;
RTC_DATA_ATTR static float last_outside_f = NAN;
RTC_DATA_ATTR static float last_outside_rh = NAN;
RTC_DATA_ATTR static float last_inside_rh = NAN;

// Weather icon state
RTC_DATA_ATTR static int32_t last_icon_id = -1;

// CRC state for change detection
RTC_DATA_ATTR static uint32_t last_footer_weather_crc = 0;
RTC_DATA_ATTR static uint32_t last_status_crc = 0;

// Published sensor values
RTC_DATA_ATTR static float last_published_inside_tempC = NAN;
RTC_DATA_ATTR static float last_published_inside_rh = NAN;
RTC_DATA_ATTR static float last_published_inside_pressureHPa = NAN;

// Display refresh state
RTC_DATA_ATTR static bool needs_full_on_boot = true;

// Display state tracking
uint16_t get_partial_counter() { return partial_counter; }

void set_partial_counter(uint16_t count) { partial_counter = count; }

void increment_partial_counter() { partial_counter++; }

void reset_partial_counter() { partial_counter = 0; }

// Temperature state tracking
float get_last_inside_f() { return last_inside_f; }

void set_last_inside_f(float temp) { last_inside_f = temp; }

float get_last_outside_f() { return last_outside_f; }

void set_last_outside_f(float temp) { last_outside_f = temp; }

float get_last_outside_rh() { return last_outside_rh; }

void set_last_outside_rh(float rh) { last_outside_rh = rh; }

float get_last_inside_rh() { return last_inside_rh; }

void set_last_inside_rh(float rh) { last_inside_rh = rh; }

// Published sensor state
float get_last_published_inside_tempC() { return last_published_inside_tempC; }

void set_last_published_inside_tempC(float temp) { last_published_inside_tempC = temp; }

float get_last_published_inside_rh() { return last_published_inside_rh; }

void set_last_published_inside_rh(float rh) { last_published_inside_rh = rh; }

float get_last_published_inside_pressureHPa() { return last_published_inside_pressureHPa; }

void set_last_published_inside_pressureHPa(float pressure) {
  last_published_inside_pressureHPa = pressure;
}

// Weather icon state
int32_t get_last_icon_id() { return last_icon_id; }

void set_last_icon_id(int32_t id) { last_icon_id = id; }

// CRC state for change detection
uint32_t get_last_footer_weather_crc() { return last_footer_weather_crc; }

void set_last_footer_weather_crc(uint32_t crc) { last_footer_weather_crc = crc; }

uint32_t get_last_status_crc() { return last_status_crc; }

void set_last_status_crc(uint32_t crc) { last_status_crc = crc; }

// Display refresh state
bool needs_full_refresh_on_boot() { return needs_full_on_boot; }

void set_needs_full_refresh_on_boot(bool needs) { needs_full_on_boot = needs; }

// Initialize all RTC state to defaults
void init_rtc_state() {
  partial_counter = 0;
  last_inside_f = NAN;
  last_outside_f = NAN;
  last_outside_rh = NAN;
  last_inside_rh = NAN;
  last_icon_id = -1;
  last_footer_weather_crc = 0;
  last_status_crc = 0;
  last_published_inside_tempC = NAN;
  last_published_inside_rh = NAN;
  last_published_inside_pressureHPa = NAN;
  needs_full_on_boot = true;
}

// Global variable for full-only mode
static bool g_full_only_mode = false;

bool get_full_only_mode() { return g_full_only_mode; }

void set_full_only_mode(bool mode) { g_full_only_mode = mode; }

// Load cached values from NVS if not already set
void nvs_load_cache_if_unset() {
  // Include system_manager for NVS functions
  extern void nvs_begin_cache();
  extern void nvs_end_cache();
  extern float nvs_load_float(const char* key, float defaultValue);
  extern uint32_t nvs_load_uint(const char* key, uint32_t defaultValue);
  extern uint16_t nvs_load_ushort(const char* key, uint16_t defaultValue);
  extern uint8_t nvs_load_uchar(const char* key, uint8_t defaultValue);

  nvs_begin_cache();

  if (!std::isfinite(last_inside_f))
    last_inside_f = nvs_load_float("li_f", NAN);
  if (!std::isfinite(last_inside_rh))
    last_inside_rh = nvs_load_float("li_rh", NAN);
  if (!std::isfinite(last_outside_f))
    last_outside_f = nvs_load_float("lo_f", NAN);
  if (!std::isfinite(last_outside_rh))
    last_outside_rh = nvs_load_float("lo_rh", NAN);
  if (last_icon_id < 0) {
    // Use UINT32_MAX (0xFFFFFFFF) as sentinel which becomes -1 when cast to int32_t
    uint32_t loaded = nvs_load_uint("icon", UINT32_MAX);
    last_icon_id = (loaded == UINT32_MAX) ? -1 : static_cast<int32_t>(loaded);
  }
  if (last_status_crc == 0)
    last_status_crc = nvs_load_uint("st_crc", 0);
  if (!std::isfinite(last_published_inside_tempC))
    last_published_inside_tempC = nvs_load_float("pi_t", NAN);
  if (!std::isfinite(last_published_inside_rh))
    last_published_inside_rh = nvs_load_float("pi_rh", NAN);
  if (!std::isfinite(last_published_inside_pressureHPa))
    last_published_inside_pressureHPa = nvs_load_float("pi_p", NAN);
  uint16_t pc = nvs_load_ushort("pcount", 0);
  if (pc > 0)
    partial_counter = pc;
  // Load render mode (0=partial, 1=full-only)
  g_full_only_mode = nvs_load_uchar("full_only", 0) != 0;

  nvs_end_cache();
}
