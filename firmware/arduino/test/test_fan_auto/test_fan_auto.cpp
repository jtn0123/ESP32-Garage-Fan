// Copyright 2026 Justin
// Native tests for fan_auto_logic.h -- the hold-at-max hysteresis thermostat.
#include <unity.h>

#include <cmath>
#include <cstring>

#include "fan/auto_logic.h"

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

// ---------------------------------------------------------------- gas boost

static const FanGasCfg kGas{true, 6, 250, 200};

static void test_gas_floor_latches_and_releases_with_hysteresis() {
  bool gh = false;
  TEST_ASSERT_EQUAL(0, fan_gas_floor(100, &gh, kGas));  // ordinary air
  TEST_ASSERT_EQUAL(0, fan_gas_floor(249, &gh, kGas));  // just under: no latch
  TEST_ASSERT_EQUAL(6, fan_gas_floor(250, &gh, kGas));  // latch at the edge
  TEST_ASSERT_EQUAL(6, fan_gas_floor(225, &gh, kGas));  // holds between bands
  TEST_ASSERT_EQUAL(6, fan_gas_floor(201, &gh, kGas));  // still above release
  TEST_ASSERT_EQUAL(0, fan_gas_floor(200, &gh, kGas));  // releases at the edge
  TEST_ASSERT_FALSE(gh);
}

static void test_gas_floor_clears_when_sensor_goes_away() {
  // A latched boost must not survive the sensor: index 0 (warming) and -1
  // (absent) both clear it, or a dead SGP41 pins the fan at boost forever.
  bool gh = false;
  TEST_ASSERT_EQUAL(6, fan_gas_floor(400, &gh, kGas));
  TEST_ASSERT_EQUAL(0, fan_gas_floor(0, &gh, kGas));
  TEST_ASSERT_FALSE(gh);
  TEST_ASSERT_EQUAL(6, fan_gas_floor(400, &gh, kGas));
  TEST_ASSERT_EQUAL(0, fan_gas_floor(-1, &gh, kGas));
  TEST_ASSERT_FALSE(gh);
}

static void test_gas_floor_disabled_clears_latch() {
  bool gh = true;  // latched when the user flips it off mid-boost
  FanGasCfg off = kGas;
  off.enabled = false;
  TEST_ASSERT_EQUAL(0, fan_gas_floor(400, &gh, off));
  TEST_ASSERT_FALSE(gh);
}

static void test_gas_floor_merges_under_thermostat() {
  // Below the floor: one step per tick toward it, then hold.
  TEST_ASSERT_EQUAL(3, fan_apply_gas_floor(/*next=*/2, /*prev=*/2, /*floor=*/6));
  TEST_ASSERT_EQUAL(6, fan_apply_gas_floor(5, 6, 6));  // thermostat wants down: floor holds
  TEST_ASSERT_EQUAL(9, fan_apply_gas_floor(9, 9, 6));  // above the floor: untouched
  TEST_ASSERT_EQUAL(2, fan_apply_gas_floor(2, 2, 0));  // no floor: untouched
  TEST_ASSERT_EQUAL(6, fan_apply_gas_floor(0, 7, 6));  // drop from above lands ON the floor
}

static void test_gas_boost_full_story() {
  // Cool garage (thermostat rests at 0), car starts inside: VOC spikes, the
  // fan climbs to the boost floor one step at a time, holds while the air is
  // bad, and ramps back down only after the index falls through the release.
  bool high = false, gh = false;
  int s = 0;
  auto tick = [&](float in_c, float out_c, int voc) {
    const int floor_speed = fan_gas_floor(voc, &gh, kGas);
    s = fan_apply_gas_floor(fan_auto_decide(in_c, out_c, s, &high, kCfg), s, floor_speed);
  };
  tick(20, 20, 120);
  TEST_ASSERT_EQUAL(0, s);
  for (int i = 0; i < 6; i++) tick(20, 20, 400);  // exhaust fills the garage
  TEST_ASSERT_EQUAL(6, s);
  for (int i = 0; i < 5; i++) tick(20, 20, 230);  // clearing, still latched
  TEST_ASSERT_EQUAL(6, s);
  for (int i = 0; i < 10; i++) tick(20, 20, 150);  // clean again
  TEST_ASSERT_EQUAL(0, s);
}

// ------------------------------------------------------------- min-run dwell

static void test_min_run_blocks_early_thermostat_release() {
  // Engage, then drop straight through the release threshold: the latch must
  // hold until the dwell is served, then release on the very next tick.
  bool high = false;
  uint16_t run = 0;
  int s = 0;
  const uint16_t kDwell = 30;  // 15 min of 30 s ticks
  for (int i = 0; i < 12; i++)
    s = fan_auto_decide(25.0f + kAbove, 25.0f, s, &high, kCfg, &run, kDwell);
  TEST_ASSERT_EQUAL(kCfg.max_speed, s);
  for (int i = 12; i < 30; i++) {  // cold again, but dwell not served
    s = fan_auto_decide(25.0f + kBelow, 25.0f, s, &high, kCfg, &run, kDwell);
    TEST_ASSERT_TRUE(high);
    TEST_ASSERT_EQUAL(kCfg.max_speed, s);
  }
  s = fan_auto_decide(25.0f + kBelow, 25.0f, s, &high, kCfg, &run, kDwell);
  TEST_ASSERT_FALSE(high);  // tick 31: dwell served, release goes through
  TEST_ASSERT_EQUAL(kCfg.max_speed - 1, s);
}

static void test_min_run_does_not_delay_engage() {
  bool high = false;
  uint16_t run = 0;
  int s = 0;
  s = fan_auto_decide(25.0f + kAbove, 25.0f, s, &high, kCfg, &run, 30);
  TEST_ASSERT_TRUE(high);  // first hot tick latches immediately
  TEST_ASSERT_EQUAL(1, s);
}

static void test_min_run_blocks_early_gas_release() {
  bool gh = false;
  uint16_t run = 0;
  TEST_ASSERT_EQUAL(6, fan_gas_floor(400, &gh, kGas, &run, 30));
  for (int i = 1; i < 30; i++) {  // air already reads clean: boost holds anyway
    TEST_ASSERT_EQUAL(6, fan_gas_floor(150, &gh, kGas, &run, 30));
    TEST_ASSERT_TRUE(gh);
  }
  TEST_ASSERT_EQUAL(0, fan_gas_floor(150, &gh, kGas, &run, 30));  // dwell served
  TEST_ASSERT_FALSE(gh);
}

static void test_min_run_never_outlives_the_sensor() {
  // The dwell must not hold a boost on a dead sensor: index <= 0 clears the
  // latch unconditionally, exactly as before the dwell existed.
  bool gh = false;
  uint16_t run = 0;
  TEST_ASSERT_EQUAL(6, fan_gas_floor(400, &gh, kGas, &run, 30));
  TEST_ASSERT_EQUAL(0, fan_gas_floor(-1, &gh, kGas, &run, 30));
  TEST_ASSERT_FALSE(gh);
  TEST_ASSERT_EQUAL(0, run);  // and the next engage starts a fresh dwell
}

static void test_min_run_reengage_restarts_the_clock() {
  bool gh = false;
  uint16_t run = 0;
  const uint16_t kDwell = 4;
  TEST_ASSERT_EQUAL(6, fan_gas_floor(300, &gh, kGas, &run, kDwell));
  for (int i = 1; i < kDwell; i++) fan_gas_floor(150, &gh, kGas, &run, kDwell);
  TEST_ASSERT_EQUAL(0, fan_gas_floor(150, &gh, kGas, &run, kDwell));  // released
  TEST_ASSERT_EQUAL(6, fan_gas_floor(300, &gh, kGas, &run, kDwell));  // round 2
  TEST_ASSERT_EQUAL(6, fan_gas_floor(150, &gh, kGas, &run, kDwell));  // held again
  TEST_ASSERT_TRUE(gh);
}

// ------------------------------------------------------------- telemetry
//
// The line that explains a decision after the fact. It is the only record of
// the dwell counter, so it has to survive absent sensors and it has to FIT:
// eventlog renders at most 80 bytes and drops the tail.

static void the_auto_line_carries_the_whole_decision() {
  char out[80];
  // 27.2 C garage, 21.1 C yard: 11 F hotter, latched high, half the dwell
  // served, heading for the user's max.
  fan_auto_log_line(out, sizeof(out), 27.2f, 21.1f, true, 15, 30, 10, false);
  TEST_ASSERT_NOT_NULL(strstr(out, "in=81.0"));
  TEST_ASSERT_NOT_NULL(strstr(out, "out=70.0"));
  TEST_ASSERT_NOT_NULL(strstr(out, "d=+11.0"));
  TEST_ASSERT_NOT_NULL(strstr(out, "latch=on"));
  TEST_ASSERT_NOT_NULL(strstr(out, "dwell=15/30"));
  TEST_ASSERT_NOT_NULL(strstr(out, "tgt=10"));
  TEST_ASSERT_NOT_NULL(strstr(out, "gas=off"));
}

static void a_missing_sensor_reads_as_absent_not_as_zero() {
  // NaN is the hold-everything contract; a line claiming 32.0 F would read as
  // a freezing garage rather than as a sensor that stopped answering.
  char out[80];
  fan_auto_log_line(out, sizeof(out), NAN, 21.1f, true, 3, 30, 10, true);
  TEST_ASSERT_NOT_NULL(strstr(out, "in=--"));
  TEST_ASSERT_NOT_NULL(strstr(out, "d=--"));
  TEST_ASSERT_NOT_NULL(strstr(out, "gas=ON"));
  TEST_ASSERT_NULL(strstr(out, "32.0"));
}

static void a_cooler_garage_shows_a_signed_negative_differential() {
  char out[80];
  fan_auto_log_line(out, sizeof(out), 20.0f, 25.0f, false, 0, 30, 0, false);
  TEST_ASSERT_NOT_NULL(strstr(out, "d=-9.0"));
  TEST_ASSERT_NOT_NULL(strstr(out, "latch=off"));
}

static void the_worst_case_auto_line_fits_the_recorders_budget() {
  char out[256];
  const int n = fan_auto_log_line(out, sizeof(out), -999.9f, 999.9f, true, 65535, 65535, 12, true);
  TEST_ASSERT_TRUE_MESSAGE(n > 0 && n < 80, "the auto line must fit eventlog's 80-byte message");
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
  RUN_TEST(test_gas_floor_latches_and_releases_with_hysteresis);
  RUN_TEST(test_gas_floor_clears_when_sensor_goes_away);
  RUN_TEST(test_gas_floor_disabled_clears_latch);
  RUN_TEST(test_gas_floor_merges_under_thermostat);
  RUN_TEST(test_gas_boost_full_story);
  RUN_TEST(test_min_run_blocks_early_thermostat_release);
  RUN_TEST(test_min_run_does_not_delay_engage);
  RUN_TEST(test_min_run_blocks_early_gas_release);
  RUN_TEST(test_min_run_never_outlives_the_sensor);
  RUN_TEST(test_min_run_reengage_restarts_the_clock);
  RUN_TEST(the_auto_line_carries_the_whole_decision);
  RUN_TEST(a_missing_sensor_reads_as_absent_not_as_zero);
  RUN_TEST(a_cooler_garage_shows_a_signed_negative_differential);
  RUN_TEST(the_worst_case_auto_line_fits_the_recorders_budget);
  return UNITY_END();
}
