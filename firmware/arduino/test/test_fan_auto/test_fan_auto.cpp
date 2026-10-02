// Copyright 2026 Justin
// Native tests for fan_auto_logic.h -- the hold-at-max hysteresis thermostat.
#include <unity.h>

#include <cmath>
#include <cstring>
#include <initializer_list>

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

// Mirrors kFanGasDefaults: 250 engage, 150 release (kGasReleaseGap = 100).
static const FanGasCfg kGas{true, 6, 250, 250 - kGasReleaseGap};

static void test_gas_floor_latches_and_releases_with_hysteresis() {
  bool gh = false;
  TEST_ASSERT_EQUAL(0, fan_gas_floor(100, &gh, kGas));  // ordinary air
  TEST_ASSERT_EQUAL(0, fan_gas_floor(249, &gh, kGas));  // just under: no latch
  TEST_ASSERT_EQUAL(6, fan_gas_floor(250, &gh, kGas));  // latch at the edge
  TEST_ASSERT_EQUAL(6, fan_gas_floor(225, &gh, kGas));  // holds between bands
  TEST_ASSERT_EQUAL(6, fan_gas_floor(200, &gh, kGas));  // the OLD release point
  TEST_ASSERT_EQUAL(6, fan_gas_floor(151, &gh, kGas));  // still above release
  TEST_ASSERT_EQUAL(0, fan_gas_floor(150, &gh, kGas));  // releases at the edge
  TEST_ASSERT_FALSE(gh);
}

static void test_release_sits_below_where_the_fan_settles_the_air() {
  // The 2026-08-23 regression this gap exists to prevent: a release point the
  // decay merely PASSES THROUGH rather than settles at. Fitted from the
  // device's own history, blowing air converges toward index 84 -- so the
  // release must sit above that (reachable) and below the level the air
  // rebounds to (or the boost quits while it is still winning).
  TEST_ASSERT_GREATER_THAN(84, kFanGasDefaults.off_index);
  TEST_ASSERT_LESS_THAN(200, kFanGasDefaults.off_index);
  TEST_ASSERT_EQUAL(100, kFanGasDefaults.on_index - kFanGasDefaults.off_index);
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

// ------------------------------------------------------------ absolute limits
//
// Winter: the garage is nearly always warmer than the yard, so the
// differential alone would vent it all the way down to outdoor temperature.

static constexpr float f2c(float f) { return (f - 32) * 5 / 9; }
static constexpr float kYard = f2c(30);  // a winter afternoon

static FanAutoCfg winter(float floor_f, float start_f) {
  FanAutoCfg cfg = kCfg;
  cfg.floor_c = std::isnan(floor_f) ? kLimitOff : f2c(floor_f);
  cfg.start_c = std::isnan(start_f) ? kLimitOff : f2c(start_f);
  return cfg;
}

static void test_limits_off_leave_the_differential_alone() {
  // The defaults must be the summer thermostat, untouched.
  TEST_ASSERT_TRUE(std::isnan(kFanAutoDefaults.floor_c));
  TEST_ASSERT_TRUE(std::isnan(kFanAutoDefaults.start_c));
  bool high = false;
  int s = 0;
  for (int i = 0; i < 20; i++) s = fan_auto_decide(f2c(40), kYard, s, &high, kCfg);
  TEST_ASSERT_EQUAL(kCfg.max_speed, s);  // a 40 F garage still vents toward 30 F
  TEST_ASSERT_EQUAL(FanLimit::kNone, fan_auto_limit(f2c(40), kYard, high, kCfg));
}

static void test_low_limit_releases_at_once_mid_dwell() {
  // The dwell holds a release back for 15 minutes; the floor does not wait.
  const FanAutoCfg cfg = winter(64, NAN);
  bool high = false;
  uint16_t run = 0;
  int s = 0;
  for (int i = 0; i < 12; i++) s = fan_auto_decide(f2c(70), kYard, s, &high, cfg, &run, 30);
  TEST_ASSERT_EQUAL(cfg.max_speed, s);
  TEST_ASSERT_EQUAL(12, run);  // well short of the dwell
  s = fan_auto_decide(f2c(64), kYard, s, &high, cfg, &run, 30);
  TEST_ASSERT_FALSE(high);
  TEST_ASSERT_EQUAL(0, run);
  TEST_ASSERT_EQUAL(cfg.max_speed - 1, s);  // still the gentle ramp down
  for (int i = 0; i < 20; i++) s = fan_auto_decide(f2c(63.5f), kYard, s, &high, cfg, &run, 30);
  TEST_ASSERT_EQUAL(cfg.min_speed, s);
}

static void test_low_limit_rests_at_the_users_rest_speed() {
  FanAutoCfg cfg = winter(64, NAN);
  cfg.min_speed = 2;
  bool high = true;
  int s = cfg.max_speed;
  for (int i = 0; i < 20; i++) s = fan_auto_decide(f2c(60), kYard, s, &high, cfg);
  TEST_ASSERT_EQUAL(2, s);
}

static void test_low_limit_needs_no_outdoor_reading() {
  // A stale weather feed holds everything else, but "the garage is already at
  // the limit" is knowable from the inside probe alone.
  const FanAutoCfg cfg = winter(64, NAN);
  bool high = true;
  TEST_ASSERT_EQUAL(8, fan_auto_decide(f2c(63), NAN, 9, &high, cfg));
  TEST_ASSERT_FALSE(high);
  high = true;  // above the floor, blind: the usual hold
  TEST_ASSERT_EQUAL(9, fan_auto_decide(f2c(70), NAN, 9, &high, cfg));
  TEST_ASSERT_TRUE(high);
}

static void test_low_limit_resumes_only_past_the_margin() {
  const FanAutoCfg cfg = winter(64, NAN);
  bool high = false;
  int s = 0;
  for (float f : {64.5f, 65.0f, 65.4f}) {  // warmer than the yard, inside the margin
    s = fan_auto_decide(f2c(f), kYard, s, &high, cfg);
    TEST_ASSERT_FALSE(high);
    TEST_ASSERT_EQUAL(FanLimit::kFloor, fan_auto_limit(f2c(f), kYard, high, cfg));
  }
  s = fan_auto_decide(f2c(65.6f), kYard, s, &high, cfg);
  TEST_ASSERT_TRUE(high);
  TEST_ASSERT_EQUAL(1, s);
  TEST_ASSERT_EQUAL(FanLimit::kNone, fan_auto_limit(f2c(65.6f), kYard, high, cfg));
}

static void test_start_gate_blocks_engage_but_not_a_running_latch() {
  const FanAutoCfg cfg = winter(NAN, 74);
  bool high = false;
  int s = 0;
  for (int i = 0; i < 10; i++) s = fan_auto_decide(f2c(72), kYard, s, &high, cfg);
  TEST_ASSERT_EQUAL(0, s);  // 42 F hotter than the yard, and left alone
  TEST_ASSERT_EQUAL(FanLimit::kStart, fan_auto_limit(f2c(72), kYard, high, cfg));
  s = fan_auto_decide(f2c(74), kYard, s, &high, cfg);
  TEST_ASSERT_TRUE(high);
  for (int i = 0; i < 20; i++) s = fan_auto_decide(f2c(70), kYard, s, &high, cfg);
  TEST_ASSERT_TRUE(high);  // back under the gate, still venting
  TEST_ASSERT_EQUAL(cfg.max_speed, s);
  TEST_ASSERT_EQUAL(FanLimit::kNone, fan_auto_limit(f2c(70), kYard, high, cfg));
}

static void test_the_binding_limit_is_the_higher_bar() {
  // A start gate set below the floor's resume point is inert, not inverted.
  const FanAutoCfg cfg = winter(64, 60);
  TEST_ASSERT_EQUAL(FanLimit::kFloor, fan_engage_blocked(f2c(62), cfg));
  TEST_ASSERT_EQUAL(FanLimit::kFloor, fan_engage_blocked(f2c(65), cfg));
  TEST_ASSERT_EQUAL(FanLimit::kNone, fan_engage_blocked(f2c(66), cfg));
  const FanAutoCfg band = winter(64, 74);
  TEST_ASSERT_EQUAL(FanLimit::kStart, fan_engage_blocked(f2c(65), band));
  TEST_ASSERT_EQUAL(FanLimit::kNone, fan_engage_blocked(f2c(74), band));
}

static void test_no_limit_is_claimed_when_the_differential_rests_anyway() {
  // Garage colder than the yard: auto would rest regardless, so naming the
  // start gate as the reason would be a false explanation.
  const FanAutoCfg cfg = winter(64, 74);
  TEST_ASSERT_EQUAL(FanLimit::kNone, fan_auto_limit(f2c(70), f2c(75), false, cfg));
  TEST_ASSERT_EQUAL(FanLimit::kNone, fan_auto_limit(NAN, kYard, false, cfg));
  TEST_ASSERT_EQUAL(FanLimit::kNone, fan_auto_limit(f2c(70), NAN, false, cfg));
}

static void test_winter_story_vents_74_down_to_64_then_rests() {
  // Both limits: the sun warms the garage past the start point, the fan
  // vents it down through the band, the floor stops it, and the margin keeps
  // it stopped while the garage drifts back up -- one cycle, no chatter.
  const FanAutoCfg cfg = winter(64, 74);
  bool high = false;
  uint16_t run = 0;
  int s = 0;
  auto tick = [&](float f) { s = fan_auto_decide(f2c(f), kYard, s, &high, cfg, &run, 30); };
  for (float f = 66; f < 74; f += 0.5f) {
    tick(f);
    TEST_ASSERT_EQUAL(0, s);  // warming through the band: left alone
  }
  for (int i = 0; i < 12; i++) tick(74.5f);
  TEST_ASSERT_EQUAL(cfg.max_speed, s);
  for (float f = 74; f > 64.2f; f -= 0.25f) {
    tick(f);
    TEST_ASSERT_TRUE(high);  // cooling through the band: keeps venting
  }
  tick(64);
  TEST_ASSERT_FALSE(high);
  for (int i = 0; i < 20; i++) tick(64);
  TEST_ASSERT_EQUAL(0, s);
  for (float f = 64; f < 73.9f; f += 0.5f) {
    tick(f);
    TEST_ASSERT_FALSE(high);  // drifting back up: rests until the start point
  }
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
  fan_auto_log_line(out, sizeof(out), 27.2f, 21.1f, true, 15, 30, 10, false, FanLimit::kNone);
  TEST_ASSERT_NOT_NULL(strstr(out, "in=81.0"));
  TEST_ASSERT_NOT_NULL(strstr(out, "out=70.0"));
  TEST_ASSERT_NOT_NULL(strstr(out, "d=+11.0"));
  TEST_ASSERT_NOT_NULL(strstr(out, "latch=on"));
  TEST_ASSERT_NOT_NULL(strstr(out, "dwell=15/30"));
  TEST_ASSERT_NOT_NULL(strstr(out, "tgt=10"));
  TEST_ASSERT_NOT_NULL(strstr(out, "gas=off"));
  TEST_ASSERT_NOT_NULL(strstr(out, "lim=-"));
}

static void the_line_names_the_limit_holding_the_fan() {
  // "latch=off d=+34.0" on a winter tape reads as a broken thermostat unless
  // the line also says the low limit is the one that switched it off.
  char out[80];
  fan_auto_log_line(out, sizeof(out), 17.8f, -1.1f, false, 0, 30, 0, false, FanLimit::kFloor);
  TEST_ASSERT_NOT_NULL(strstr(out, "lim=floor"));
  fan_auto_log_line(out, sizeof(out), 21.0f, -1.1f, false, 0, 30, 0, false, FanLimit::kStart);
  TEST_ASSERT_NOT_NULL(strstr(out, "lim=start"));
}

static void a_missing_sensor_reads_as_absent_not_as_zero() {
  // NaN is the hold-everything contract; a line claiming 32.0 F would read as
  // a freezing garage rather than as a sensor that stopped answering.
  char out[80];
  fan_auto_log_line(out, sizeof(out), NAN, 21.1f, true, 3, 30, 10, true, FanLimit::kNone);
  TEST_ASSERT_NOT_NULL(strstr(out, "in=--"));
  TEST_ASSERT_NOT_NULL(strstr(out, "d=--"));
  TEST_ASSERT_NOT_NULL(strstr(out, "gas=ON"));
  TEST_ASSERT_NULL(strstr(out, "32.0"));
}

static void a_cooler_garage_shows_a_signed_negative_differential() {
  char out[80];
  fan_auto_log_line(out, sizeof(out), 20.0f, 25.0f, false, 0, 30, 0, false, FanLimit::kNone);
  TEST_ASSERT_NOT_NULL(strstr(out, "d=-9.0"));
  TEST_ASSERT_NOT_NULL(strstr(out, "latch=off"));
}

static void the_worst_case_auto_line_fits_the_recorders_budget() {
  char out[256];
  for (FanLimit lim : {FanLimit::kNone, FanLimit::kFloor, FanLimit::kStart}) {
    const int n =
        fan_auto_log_line(out, sizeof(out), -999.9f, 999.9f, true, 65535, 65535, 12, true, lim);
    TEST_ASSERT_TRUE_MESSAGE(n > 0 && n < 80, "the auto line must fit eventlog's 80-byte message");
  }
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
  RUN_TEST(test_release_sits_below_where_the_fan_settles_the_air);
  RUN_TEST(test_gas_floor_clears_when_sensor_goes_away);
  RUN_TEST(test_gas_floor_disabled_clears_latch);
  RUN_TEST(test_gas_floor_merges_under_thermostat);
  RUN_TEST(test_gas_boost_full_story);
  RUN_TEST(test_min_run_blocks_early_thermostat_release);
  RUN_TEST(test_min_run_does_not_delay_engage);
  RUN_TEST(test_min_run_blocks_early_gas_release);
  RUN_TEST(test_min_run_never_outlives_the_sensor);
  RUN_TEST(test_min_run_reengage_restarts_the_clock);
  RUN_TEST(test_limits_off_leave_the_differential_alone);
  RUN_TEST(test_low_limit_releases_at_once_mid_dwell);
  RUN_TEST(test_low_limit_rests_at_the_users_rest_speed);
  RUN_TEST(test_low_limit_needs_no_outdoor_reading);
  RUN_TEST(test_low_limit_resumes_only_past_the_margin);
  RUN_TEST(test_start_gate_blocks_engage_but_not_a_running_latch);
  RUN_TEST(test_the_binding_limit_is_the_higher_bar);
  RUN_TEST(test_no_limit_is_claimed_when_the_differential_rests_anyway);
  RUN_TEST(test_winter_story_vents_74_down_to_64_then_rests);
  RUN_TEST(the_auto_line_carries_the_whole_decision);
  RUN_TEST(the_line_names_the_limit_holding_the_fan);
  RUN_TEST(a_missing_sensor_reads_as_absent_not_as_zero);
  RUN_TEST(a_cooler_garage_shows_a_signed_negative_differential);
  RUN_TEST(the_worst_case_auto_line_fits_the_recorders_budget);
  return UNITY_END();
}
