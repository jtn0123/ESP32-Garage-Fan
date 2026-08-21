// Copyright 2026 Justin
// Native tests for sensors/rolling.h -- the rolling mean that stands between
// one flaky sensor reading and an auto-mode latch flip.
#include <unity.h>

#include <cmath>

#include "sensors/rolling.h"

using sensors::RollingAvg;

void setUp() {}
void tearDown() {}

static void test_empty_window_is_nan() {
  RollingAvg<30> a;
  TEST_ASSERT_TRUE(a.empty());
  TEST_ASSERT_TRUE(std::isnan(a.avg()));
}

static void test_answers_from_first_sample() {
  // A fresh boot must not spend 30 s with no opinion.
  RollingAvg<30> a;
  a.push(21.5f);
  TEST_ASSERT_FALSE(a.empty());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.5f, a.avg());
}

static void test_partial_window_averages_what_it_has() {
  RollingAvg<30> a;
  a.push(10.0f);
  a.push(20.0f);
  a.push(30.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, a.avg());
}

static void test_full_window_drops_oldest() {
  RollingAvg<3> a;
  a.push(10.0f);
  a.push(20.0f);
  a.push(30.0f);
  a.push(40.0f);  // evicts the 10
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.0f, a.avg());
}

static void test_one_outlier_cannot_swing_a_full_window() {
  // The point of the whole file: a single faulty reading moves a 30-sample
  // mean by at most outlier/30, nowhere near an auto-mode threshold jump.
  RollingAvg<30> a;
  for (int i = 0; i < 30; i++) a.push(30.0f);
  a.push(45.0f);  // one 15-degree glitch
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.5f, a.avg());
}

static void test_reset_forgets_everything() {
  RollingAvg<3> a;
  a.push(3.9f);
  a.push(3.8f);
  a.reset();
  TEST_ASSERT_TRUE(a.empty());
  TEST_ASSERT_TRUE(std::isnan(a.avg()));
  // And it refills cleanly from scratch.
  a.push(4.1f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.1f, a.avg());
}

static void test_long_run_tracks_a_step_change_fully() {
  // After a real (sustained) change the mean must converge on it exactly,
  // not hover -- smoothing delays truth, it must never distort it.
  RollingAvg<30> a;
  for (int i = 0; i < 100; i++) a.push(200.0f);
  for (int i = 0; i < 30; i++) a.push(260.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 260.0f, a.avg());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_window_is_nan);
  RUN_TEST(test_answers_from_first_sample);
  RUN_TEST(test_partial_window_averages_what_it_has);
  RUN_TEST(test_full_window_drops_oldest);
  RUN_TEST(test_one_outlier_cannot_swing_a_full_window);
  RUN_TEST(test_reset_forgets_everything);
  RUN_TEST(test_long_run_tracks_a_step_change_fully);
  return UNITY_END();
}
