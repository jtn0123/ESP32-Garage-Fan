// Copyright 2026 Justin
// Native tests for fan_auto_logic.h -- the auto-mode differential thermostat.
#include <unity.h>

#include "fan_auto_logic.h"

static const FanAutoCfg kCfg = kFanAutoDefaults;  // min 1, max 9, 0.3C, 4C

void setUp() {}
void tearDown() {}

static void test_outside_hotter_steps_down_to_min() {
  int s = 9;
  for (int i = 0; i < 20; i++) s = fan_auto_decide(30.0f, 35.0f, s, kCfg);
  TEST_ASSERT_EQUAL(kCfg.min_speed, s);
}

static void test_inside_much_hotter_ramps_to_max_one_step_per_tick() {
  int s = 1;
  s = fan_auto_decide(35.0f, 25.0f, s, kCfg);
  TEST_ASSERT_EQUAL(2, s);  // exactly one step, not a jump
  for (int i = 0; i < 20; i++) s = fan_auto_decide(35.0f, 25.0f, s, kCfg);
  TEST_ASSERT_EQUAL(kCfg.max_speed, s);  // converges to the ceiling, never 10+
}

static void test_max_speed_is_respected() {
  FanAutoCfg cfg = kCfg;
  cfg.max_speed = 5;
  int s = 1;
  for (int i = 0; i < 20; i++) s = fan_auto_decide(40.0f, 20.0f, s, cfg);
  TEST_ASSERT_EQUAL(5, s);
}

static void test_missing_data_holds_current_speed() {
  TEST_ASSERT_EQUAL(7, fan_auto_decide(NAN, 25.0f, 7, kCfg));
  TEST_ASSERT_EQUAL(7, fan_auto_decide(30.0f, NAN, 7, kCfg));
  TEST_ASSERT_EQUAL(0, fan_auto_decide(NAN, NAN, 0, kCfg));
}

static void test_equal_temperatures_idle_at_min() {
  int s = 6;
  for (int i = 0; i < 10; i++) s = fan_auto_decide(28.0f, 28.0f, s, kCfg);
  TEST_ASSERT_EQUAL(kCfg.min_speed, s);
}

static void test_converged_speed_is_stable_no_hunting() {
  // A mid-range delta lands on one target; once reached, repeated ticks with
  // the same readings must not oscillate.
  int s = 1;
  for (int i = 0; i < 20; i++) s = fan_auto_decide(30.0f, 28.0f, s, kCfg);
  const int settled = s;
  for (int i = 0; i < 10; i++) {
    s = fan_auto_decide(30.0f, 28.0f, s, kCfg);
    TEST_ASSERT_EQUAL(settled, s);
  }
  TEST_ASSERT_TRUE(settled > kCfg.min_speed && settled < kCfg.max_speed);
}

static void test_proportional_zone_is_monotonic() {
  // Bigger delta must never produce a smaller settled speed.
  int last = 0;
  for (float delta = 0.5f; delta <= 6.0f; delta += 0.5f) {
    int s = 1;
    for (int i = 0; i < 30; i++)
      s = fan_auto_decide(25.0f + delta, 25.0f, s, kCfg);
    TEST_ASSERT_TRUE(s >= last);
    last = s;
  }
  TEST_ASSERT_EQUAL(kCfg.max_speed, last);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_outside_hotter_steps_down_to_min);
  RUN_TEST(test_inside_much_hotter_ramps_to_max_one_step_per_tick);
  RUN_TEST(test_max_speed_is_respected);
  RUN_TEST(test_missing_data_holds_current_speed);
  RUN_TEST(test_equal_temperatures_idle_at_min);
  RUN_TEST(test_converged_speed_is_stable_no_hunting);
  RUN_TEST(test_proportional_zone_is_monotonic);
  return UNITY_END();
}
