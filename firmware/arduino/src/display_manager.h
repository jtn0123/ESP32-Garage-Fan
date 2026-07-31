#pragma once

// Display management module - extracted from main.cpp
// Handles all display rendering, partial updates, and UI operations

#include "config.h"
#if USE_DISPLAY

#include <Arduino.h>
#include <cstdint>
#include "common_types.h"

// Forward declarations
struct BatteryStatus;
struct OutsideReadings;

// Main display functions
// clear_panel: spend a full refresh blanking the panel before use. The
// always-on boot path passes false - its first real draw follows within
// seconds and each full refresh costs ~3 s of watchdog budget plus one of
// the panel's rated cycles. Diagnostic/debug builds keep the clear.
void display_manager_init(bool clear_panel = true);
void full_refresh();
void smoke_full_window_test();

// Partial update functions

// Drawing functions
void draw_static_chrome();
void draw_header_time(const char* time_str);
void draw_header_time_direct(const char* time_str);
void draw_status_line(const BatteryStatus& bs, const char* ip_cstr);
void draw_status_line_direct(const BatteryStatus& bs, const char* ip_cstr);
void draw_values(const char* in_temp_f, const char* in_rh, const char* out_temp_f,
                 const char* out_rh, const char* condition, const char* wind_str,
                 const char* in_pressure_str, const char* out_pressure_str);

// Weather icon functions
void draw_weather_icon_region_at(int16_t x, int16_t y, int16_t w, int16_t h, const char* weather);
void draw_weather_icon_region_at_from_outside(int16_t x, int16_t y, int16_t w, int16_t h,
                                              const OutsideReadings& o);

// Utility functions
void make_short_condition_cstr(const char* weather, char* out, size_t out_size);
int16_t text_width_default_font(const char* s, uint8_t size);
float get_last_outside_f();

// Template functions (must be in header)
// Implemented directly in main.cpp for now to avoid complex refactoring

// UI spec rendering (if enabled)
#if USE_UI_SPEC
void draw_from_spec_full(uint8_t variantId);
const int* rect_ptr_by_id(uint8_t rid);
#endif

// Dev mode display tick (if enabled)
#if DEV_NO_SLEEP
#endif

// Global display object access
// Note: The actual display type is determined at compile time in main.cpp
// We don't expose it directly to avoid circular dependencies

#endif  // USE_DISPLAY
