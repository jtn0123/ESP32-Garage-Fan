// Unit tests for the state_manager module.
//
// This test compiles the REAL src/state_manager.cpp (see env:native_state_manager
// in platformio.ini) rather than a copy of it, so it cannot drift from the
// implementation.
//
// The previous version of this file was written against an API that never
// existed -- set_last_inside_temp_f(), get_last_battery_pct(), get_has_changed(),
// set_wake_time_ms() and the wake-count functions are not part of
// state_manager.h (wake counts live in system_manager). It could not compile, and
// because CI only ran test_rounding, nobody found out. It has been rewritten
// against the real surface.

#include <unity.h>
#include <cmath>
#include <cstdint>

#include "../../src/state_manager.h"

// state_manager.cpp's nvs_load_cache_if_unset() calls these; they are defined in
// system_manager.cpp, which pulls in Preferences/NVS and cannot build on the
// host. Stubbing them here lets the cache-load logic itself be tested, with the
// stub standing in for what NVS would have returned.
namespace {
bool g_nvs_open = false;
bool g_nvs_closed = false;

float g_nvs_float = NAN;
uint32_t g_nvs_uint = 0;
uint16_t g_nvs_ushort = 0;
uint8_t g_nvs_uchar = 0;
bool g_nvs_return_defaults = true;
}  // namespace

void nvs_begin_cache() {
  g_nvs_open = true;
}
void nvs_end_cache() {
  g_nvs_closed = true;
}
float nvs_load_float(const char*, float defaultValue) {
  return g_nvs_return_defaults ? defaultValue : g_nvs_float;
}
uint32_t nvs_load_uint(const char*, uint32_t defaultValue) {
  return g_nvs_return_defaults ? defaultValue : g_nvs_uint;
}
uint16_t nvs_load_ushort(const char*, uint16_t defaultValue) {
  return g_nvs_return_defaults ? defaultValue : g_nvs_ushort;
}
uint8_t nvs_load_uchar(const char*, uint8_t defaultValue) {
  return g_nvs_return_defaults ? defaultValue : g_nvs_uchar;
}

void setUp() {
  init_rtc_state();
  set_full_only_mode(false);
  g_nvs_open = false;
  g_nvs_closed = false;
  g_nvs_return_defaults = true;
}

void tearDown() {}

// === init_rtc_state ==========================================================

void test_init_rtc_state_sets_defaults() {
  // Dirty every field, then confirm init puts them all back.
  set_partial_counter(9);
  set_last_inside_f(70.0f);
  set_last_outside_f(50.0f);
  set_last_inside_rh(40.0f);
  set_last_outside_rh(60.0f);
  set_last_icon_id(7);
  set_last_footer_weather_crc(0xAAAA);
  set_last_status_crc(0xBBBB);
  set_last_published_inside_tempC(21.0f);
  set_last_published_inside_rh(45.0f);
  set_last_published_inside_pressureHPa(1013.0f);
  set_needs_full_refresh_on_boot(false);

  init_rtc_state();

  TEST_ASSERT_EQUAL_UINT16(0, get_partial_counter());
  TEST_ASSERT_TRUE(std::isnan(get_last_inside_f()));
  TEST_ASSERT_TRUE(std::isnan(get_last_outside_f()));
  TEST_ASSERT_TRUE(std::isnan(get_last_inside_rh()));
  TEST_ASSERT_TRUE(std::isnan(get_last_outside_rh()));
  TEST_ASSERT_EQUAL_INT32(-1, get_last_icon_id());
  TEST_ASSERT_EQUAL_UINT32(0, get_last_footer_weather_crc());
  TEST_ASSERT_EQUAL_UINT32(0, get_last_status_crc());
  TEST_ASSERT_TRUE(std::isnan(get_last_published_inside_tempC()));
  TEST_ASSERT_TRUE(std::isnan(get_last_published_inside_rh()));
  TEST_ASSERT_TRUE(std::isnan(get_last_published_inside_pressureHPa()));
  // A fresh boot must repaint the whole panel; partial updates onto an unknown
  // image would leave stale pixels.
  TEST_ASSERT_TRUE(needs_full_refresh_on_boot());
}

// === partial counter =========================================================

void test_partial_counter_set_get() {
  set_partial_counter(5);
  TEST_ASSERT_EQUAL_UINT16(5, get_partial_counter());
}

void test_partial_counter_increment_and_reset() {
  reset_partial_counter();
  TEST_ASSERT_EQUAL_UINT16(0, get_partial_counter());

  increment_partial_counter();
  increment_partial_counter();
  increment_partial_counter();
  TEST_ASSERT_EQUAL_UINT16(3, get_partial_counter());

  reset_partial_counter();
  TEST_ASSERT_EQUAL_UINT16(0, get_partial_counter());
}

void test_partial_counter_wraps_at_uint16() {
  // The counter is uint16_t and only ever compared against FULL_REFRESH_EVERY,
  // so wrapping is harmless -- but pin the behaviour so a type change is noticed.
  set_partial_counter(65535);
  increment_partial_counter();
  TEST_ASSERT_EQUAL_UINT16(0, get_partial_counter());
}

// === temperature / humidity state ============================================

void test_inside_outside_temp_round_trip() {
  set_last_inside_f(72.5f);
  set_last_outside_f(48.25f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 72.5f, get_last_inside_f());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 48.25f, get_last_outside_f());
}

void test_humidity_round_trip() {
  set_last_inside_rh(41.0f);
  set_last_outside_rh(88.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 41.0f, get_last_inside_rh());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 88.0f, get_last_outside_rh());
}

void test_negative_and_zero_temps_are_preserved() {
  // 0 must not be confused with "unset" -- NaN is the unset marker here.
  set_last_outside_f(0.0f);
  TEST_ASSERT_FALSE(std::isnan(get_last_outside_f()));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, get_last_outside_f());

  set_last_outside_f(-13.5f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -13.5f, get_last_outside_f());
}

// === published sensor state ==================================================

void test_published_values_round_trip() {
  set_last_published_inside_tempC(21.75f);
  set_last_published_inside_rh(46.0f);
  set_last_published_inside_pressureHPa(1008.5f);

  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.75f, get_last_published_inside_tempC());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 46.0f, get_last_published_inside_rh());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1008.5f, get_last_published_inside_pressureHPa());
}

// === weather icon / CRC state ================================================

void test_icon_id_round_trip_and_unset_sentinel() {
  TEST_ASSERT_EQUAL_INT32(-1, get_last_icon_id());  // from setUp's init
  set_last_icon_id(42);
  TEST_ASSERT_EQUAL_INT32(42, get_last_icon_id());
  set_last_icon_id(0);  // 0 is a valid icon id, distinct from unset
  TEST_ASSERT_EQUAL_INT32(0, get_last_icon_id());
}

void test_crc_state_round_trip() {
  set_last_footer_weather_crc(0xDEADBEEF);
  set_last_status_crc(0x12345678);
  TEST_ASSERT_EQUAL_UINT32(0xDEADBEEF, get_last_footer_weather_crc());
  TEST_ASSERT_EQUAL_UINT32(0x12345678, get_last_status_crc());
}

// === display mode flags ======================================================

void test_full_refresh_on_boot_flag() {
  set_needs_full_refresh_on_boot(false);
  TEST_ASSERT_FALSE(needs_full_refresh_on_boot());
  set_needs_full_refresh_on_boot(true);
  TEST_ASSERT_TRUE(needs_full_refresh_on_boot());
}

void test_full_only_mode_flag() {
  set_full_only_mode(true);
  TEST_ASSERT_TRUE(get_full_only_mode());
  set_full_only_mode(false);
  TEST_ASSERT_FALSE(get_full_only_mode());
}

// === NVS cache load ==========================================================

void test_cache_load_opens_and_closes_nvs() {
  nvs_load_cache_if_unset();
  TEST_ASSERT_TRUE(g_nvs_open);
  TEST_ASSERT_TRUE(g_nvs_closed);
}

void test_cache_load_fills_only_unset_fields() {
  // Inside temp already known; outside temp is not. Only the latter should be
  // taken from storage -- that is the whole point of "_if_unset".
  set_last_inside_f(70.0f);
  g_nvs_return_defaults = false;
  g_nvs_float = 33.0f;
  g_nvs_uint = UINT32_MAX;

  nvs_load_cache_if_unset();

  TEST_ASSERT_FLOAT_WITHIN(0.001f, 70.0f, get_last_inside_f());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 33.0f, get_last_outside_f());
}

void test_cache_load_icon_sentinel_maps_to_unset() {
  // Storage returns UINT32_MAX to mean "no icon saved"; it must come back as -1
  // rather than as 4294967295 cast into an int32_t.
  g_nvs_return_defaults = false;
  g_nvs_uint = UINT32_MAX;
  g_nvs_float = NAN;

  nvs_load_cache_if_unset();

  TEST_ASSERT_EQUAL_INT32(-1, get_last_icon_id());
}

void test_cache_load_restores_real_icon_id() {
  g_nvs_return_defaults = false;
  g_nvs_uint = 5;
  g_nvs_float = NAN;

  nvs_load_cache_if_unset();

  TEST_ASSERT_EQUAL_INT32(5, get_last_icon_id());
}

void test_cache_load_partial_counter_only_when_nonzero() {
  // A stored 0 is indistinguishable from "never saved", so the in-RAM value wins.
  set_partial_counter(4);
  g_nvs_return_defaults = false;
  g_nvs_ushort = 0;
  g_nvs_float = NAN;
  g_nvs_uint = UINT32_MAX;

  nvs_load_cache_if_unset();
  TEST_ASSERT_EQUAL_UINT16(4, get_partial_counter());

  g_nvs_ushort = 11;
  nvs_load_cache_if_unset();
  TEST_ASSERT_EQUAL_UINT16(11, get_partial_counter());
}

void test_cache_load_restores_full_only_mode() {
  g_nvs_return_defaults = false;
  g_nvs_uchar = 1;
  g_nvs_float = NAN;
  g_nvs_uint = UINT32_MAX;

  nvs_load_cache_if_unset();
  TEST_ASSERT_TRUE(get_full_only_mode());

  g_nvs_uchar = 0;
  nvs_load_cache_if_unset();
  TEST_ASSERT_FALSE(get_full_only_mode());
}

// Native entry point. These tests run on platform = native, which has no
// Arduino setup()/loop() runtime to call into.
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  RUN_TEST(test_init_rtc_state_sets_defaults);

  RUN_TEST(test_partial_counter_set_get);
  RUN_TEST(test_partial_counter_increment_and_reset);
  RUN_TEST(test_partial_counter_wraps_at_uint16);

  RUN_TEST(test_inside_outside_temp_round_trip);
  RUN_TEST(test_humidity_round_trip);
  RUN_TEST(test_negative_and_zero_temps_are_preserved);

  RUN_TEST(test_published_values_round_trip);

  RUN_TEST(test_icon_id_round_trip_and_unset_sentinel);
  RUN_TEST(test_crc_state_round_trip);

  RUN_TEST(test_full_refresh_on_boot_flag);
  RUN_TEST(test_full_only_mode_flag);

  RUN_TEST(test_cache_load_opens_and_closes_nvs);
  RUN_TEST(test_cache_load_fills_only_unset_fields);
  RUN_TEST(test_cache_load_icon_sentinel_maps_to_unset);
  RUN_TEST(test_cache_load_restores_real_icon_id);
  RUN_TEST(test_cache_load_partial_counter_only_when_nonzero);
  RUN_TEST(test_cache_load_restores_full_only_mode);

  return UNITY_END();
}
