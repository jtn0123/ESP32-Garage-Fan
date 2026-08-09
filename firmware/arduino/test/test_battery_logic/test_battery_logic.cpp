// Copyright 2026 Justin
// Native tests for sensors/battery_logic.h -- the sliding window, the sticky
// charging verdict and the runtime ETA. The first two cases are regression
// tests for a real field bug (2026-08): an unknown RSOC was stored as 0,
// which read as a huge charge step and flipped the verdict, dragging the
// temperature offset ~12 C with it.
#include <unity.h>

#include <cmath>

#include "sensors/battery_logic.h"

using battery::eta_hours;
using battery::judge_charging;
using battery::kWinLen;
using battery::Window;

void setUp() {}
void tearDown() {}

static void test_unknown_percent_seeds_nan_not_zero() {
  Window w;
  w.push(3.78f, NAN);
  TEST_ASSERT_TRUE(std::isnan(w.pct[0]));  // the bug wrote 0.0 here
  TEST_ASSERT_EQUAL_FLOAT(3.78f, w.v[0]);
}

static void test_nan_seed_does_not_fake_a_charge() {
  // The field failure: window [NAN-as-0, ..., 29] looked like +29 pct in
  // minutes -> "charging". With NAN seeds the percent contributes no signal
  // and flat voltage must hold the verdict at false.
  Window w;
  w.push(3.776f, NAN);
  w.push(3.776f, NAN);
  w.push(3.776f, 29.0f);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, w.drain_pct());  // no signal, not -29
  TEST_ASSERT_FALSE(judge_charging(w, 3.776f, false));
}

static void test_transient_failed_read_carries_last_percent() {
  Window w;
  w.push(3.80f, 50.0f);
  w.push(3.80f, NAN);  // one missed RSOC read
  TEST_ASSERT_EQUAL_FLOAT(50.0f, w.pct[1]);
}

static void test_verdict_holds_below_three_points() {
  Window w;
  w.push(4.0f, 80);
  w.push(4.1f, 81);
  TEST_ASSERT_TRUE(judge_charging(w, 4.1f, true));
  TEST_ASSERT_FALSE(judge_charging(w, 4.1f, false));
}

static void test_rising_voltage_slope_enters_charging() {
  Window w;
  w.push(3.80f, NAN);
  w.push(3.82f, NAN);
  w.push(3.84f, NAN);  // +240 mV/h across 10 min, way past the +5 mV/h bar
  TEST_ASSERT_TRUE(judge_charging(w, 3.84f, false));
}

static void test_falling_voltage_slope_leaves_charging() {
  Window w;
  w.push(3.84f, NAN);
  w.push(3.82f, NAN);
  w.push(3.80f, NAN);
  TEST_ASSERT_FALSE(judge_charging(w, 3.80f, true));
}

static void test_float_voltage_means_charging_even_when_flat() {
  Window w;
  w.push(4.18f, NAN);
  w.push(4.18f, NAN);
  w.push(4.18f, NAN);
  TEST_ASSERT_TRUE(judge_charging(w, 4.18f, false));
}

static void test_plateau_is_sticky_both_ways() {
  Window w;
  w.push(3.90f, 70.0f);
  w.push(3.90f, 70.1f);  // jitter inside every threshold
  w.push(3.90f, 69.9f);
  TEST_ASSERT_TRUE(judge_charging(w, 3.90f, true));
  TEST_ASSERT_FALSE(judge_charging(w, 3.90f, false));
}

static void test_rsoc_drain_leaves_charging() {
  Window w;
  w.push(3.90f, 70.0f);
  w.push(3.90f, 69.7f);
  w.push(3.90f, 69.5f);  // -0.5 pct across the window, volts flat
  TEST_ASSERT_FALSE(judge_charging(w, 3.90f, true));
}

static void test_rsoc_rise_enters_charging() {
  Window w;
  w.push(3.90f, 70.0f);
  w.push(3.90f, 70.3f);
  w.push(3.90f, 70.5f);
  TEST_ASSERT_TRUE(judge_charging(w, 3.90f, false));
}

static void test_window_evicts_at_capacity() {
  Window w;
  for (int i = 0; i < kWinLen + 2; i++)
    w.push(3.5f + i * 0.01f, static_cast<float>(i));
  TEST_ASSERT_EQUAL_UINT8(kWinLen, w.n);
  TEST_ASSERT_EQUAL_FLOAT(2.0f, w.pct[0]);  // the first two points aged out
}

static void test_eta_needs_six_points_and_discharge() {
  Window w;
  for (int i = 0; i < 5; i++)
    w.push(3.9f, 90.0f - i);
  TEST_ASSERT_TRUE(std::isnan(eta_hours(w, 86, false)));  // only 5 points
  w.push(3.9f, 85.0f);
  TEST_ASSERT_TRUE(std::isnan(eta_hours(w, 85, true)));  // charging -> no ETA
  // 6 points, 5 pct drained over 25 min (12 pct/h) from 85 pct -> ~7.1 h.
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 85.0f / 12.0f, eta_hours(w, 85, false));
}

static void test_eta_flat_or_unknown_rsoc_is_nan() {
  Window flat;
  for (int i = 0; i < 8; i++)
    flat.push(3.9f, 80.0f);  // no drain at all
  TEST_ASSERT_TRUE(std::isnan(eta_hours(flat, 80, false)));
  Window unknown;
  for (int i = 0; i < 8; i++)
    unknown.push(3.9f, NAN);  // RSOC never answered
  TEST_ASSERT_TRUE(std::isnan(eta_hours(unknown, 80, false)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_unknown_percent_seeds_nan_not_zero);
  RUN_TEST(test_nan_seed_does_not_fake_a_charge);
  RUN_TEST(test_transient_failed_read_carries_last_percent);
  RUN_TEST(test_verdict_holds_below_three_points);
  RUN_TEST(test_rising_voltage_slope_enters_charging);
  RUN_TEST(test_falling_voltage_slope_leaves_charging);
  RUN_TEST(test_float_voltage_means_charging_even_when_flat);
  RUN_TEST(test_plateau_is_sticky_both_ways);
  RUN_TEST(test_rsoc_drain_leaves_charging);
  RUN_TEST(test_rsoc_rise_enters_charging);
  RUN_TEST(test_window_evicts_at_capacity);
  RUN_TEST(test_eta_needs_six_points_and_discharge);
  RUN_TEST(test_eta_flat_or_unknown_rsoc_is_nan);
  return UNITY_END();
}
