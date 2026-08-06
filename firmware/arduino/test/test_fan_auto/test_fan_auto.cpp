// Copyright 2026 Justin
// Native tests for fan_auto_logic.h -- the hold-at-max hysteresis thermostat.
#include <unity.h>

#include "fan_auto_logic.h"

static const FanAutoCfg kCfg = kFanAutoDefaults;  // min 0, max 9, 2.5F/1.5F

void setUp() {}
void tearDown() {}

// Delta helpers: thresholds are in C internally; use deltas comfortably
// beyond/inside them so float rounding can't flip a comparison.
static constexpr float kAbove = 2.0f;    // > on_delta_c (1.39 C)
static constexpr float kBetween = 1.1f;  // between off (0.83) and on (1.39)
static constexpr float kBelow = 0.5f;    // < off_delta_c

static void test_hot_garage_ramps_to_max_one_step_per_tick() {
  bool high = false;
  int s = 0;
  s = fan_auto_decide(25.0f + kAbove, 25.0f, s, &high, kCfg);
  TEST_ASSERT_EQUAL(1, s);  // exactly one step, not a jump
  TEST_ASSERT_TRUE(high);
  for (int i = 0; i < 20; i++) s = fan_auto_decide(25.0f + kAbove, 25.0f, s, &high, kCfg);
  TEST_ASSERT_EQUAL(kCfg.max_speed, s);  // full blast, never 10+
}

static void test_holds_max_between_thresholds() {
  // The complaint that started this: garage still hotter, just less so.
  // Once latched high, a shrinking-but-positive delta must NOT slow the fan.
  bool high = true;
  int s = kCfg.max_speed;
  for (int i = 0; i < 20; i++) s = fan_auto_decide(25.0f + kBetween, 25.0f, s, &high, kCfg);
  TEST_ASSERT_EQUAL(kCfg.max_speed, s);
  TEST_ASSERT_TRUE(high);
}

static void test_releases_to_min_below_off_threshold() {
  bool high = true;
  int s = kCfg.max_speed;
  for (int i = 0; i < 20; i++) s = fan_auto_decide(25.0f + kBelow, 25.0f, s, &high, kCfg);
  TEST_ASSERT_EQUAL(kCfg.min_speed, s);  // default min 0 = off
  TEST_ASSERT_FALSE(high);
}

static void test_outside_hotter_rests_at_min() {
  bool high = true;
  int s = 9;
  for (int i = 0; i < 20; i++) s = fan_auto_decide(28.0f, 33.0f, s, &high, kCfg);
  TEST_ASSERT_EQUAL(kCfg.min_speed, s);
}

static void test_low_state_holds_between_thresholds() {
  // Rising back into the dead zone must not re-engage until >= on threshold.
  bool high = false;
  int s = kCfg.min_speed;
  for (int i = 0; i < 20; i++) s = fan_auto_decide(25.0f + kBetween, 25.0f, s, &high, kCfg);
  TEST_ASSERT_EQUAL(kCfg.min_speed, s);
  TEST_ASSERT_FALSE(high);
}

static void test_user_min_speed_is_the_rest_floor() {
  FanAutoCfg cfg = kCfg;
  cfg.min_speed = 3;
  bool high = true;
  int s = cfg.max_speed;
  for (int i = 0; i < 20; i++) s = fan_auto_decide(25.0f + kBelow, 25.0f, s, &high, cfg);
  TEST_ASSERT_EQUAL(3, s);
}

static void test_user_max_speed_is_respected() {
  FanAutoCfg cfg = kCfg;
  cfg.max_speed = 3;  // the "3 to off" final-setup example
  bool high = false;
  int s = 0;
  for (int i = 0; i < 20; i++) s = fan_auto_decide(40.0f, 20.0f, s, &high, cfg);
  TEST_ASSERT_EQUAL(3, s);
}

static void test_missing_data_holds_speed_and_latch() {
  bool high = true;
  TEST_ASSERT_EQUAL(7, fan_auto_decide(NAN, 25.0f, 7, &high, kCfg));
  TEST_ASSERT_TRUE(high);
  high = false;
  TEST_ASSERT_EQUAL(7, fan_auto_decide(30.0f, NAN, 7, &high, kCfg));
  TEST_ASSERT_FALSE(high);
  TEST_ASSERT_EQUAL(0, fan_auto_decide(NAN, NAN, 0, &high, kCfg));
}

static void test_full_cycle_hot_afternoon_to_cool_evening() {
  // Story test: garage bakes, fan holds max through the whole cooldown,
  // drops to rest only near equilibrium, stays down in the dead zone.
  bool high = false;
  int s = 0;
  const float out = 24.0f;
  for (int i = 0; i < 15; i++) s = fan_auto_decide(out + 4.0f, out, s, &high, kCfg);
  TEST_ASSERT_EQUAL(kCfg.max_speed, s);
  for (float d = 4.0f; d > 0.9f; d -= 0.2f) {  // cooling, still hot-ish
    s = fan_auto_decide(out + d, out, s, &high, kCfg);
    TEST_ASSERT_EQUAL(kCfg.max_speed, s);  // no premature slowdown, ever
  }
  for (int i = 0; i < 15; i++)  // near-equalized -> rest
    s = fan_auto_decide(out + 0.4f, out, s, &high, kCfg);
  TEST_ASSERT_EQUAL(kCfg.min_speed, s);
  for (int i = 0; i < 10; i++)  // dead zone after release: stays at rest
    s = fan_auto_decide(out + kBetween, out, s, &high, kCfg);
  TEST_ASSERT_EQUAL(kCfg.min_speed, s);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_hot_garage_ramps_to_max_one_step_per_tick);
  RUN_TEST(test_holds_max_between_thresholds);
  RUN_TEST(test_releases_to_min_below_off_threshold);
  RUN_TEST(test_outside_hotter_rests_at_min);
  RUN_TEST(test_low_state_holds_between_thresholds);
  RUN_TEST(test_user_min_speed_is_the_rest_floor);
  RUN_TEST(test_user_max_speed_is_respected);
  RUN_TEST(test_missing_data_holds_speed_and_latch);
  RUN_TEST(test_full_cycle_hot_afternoon_to_cool_evening);
  return UNITY_END();
}
