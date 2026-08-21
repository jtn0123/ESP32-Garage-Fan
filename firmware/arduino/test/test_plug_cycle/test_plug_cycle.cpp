// Copyright 2026 Justin
// Native tests for net/plug_cycle.h -- the fan-cycling profile. A poll is
// 15 s; the default profile needs 4 confirmed flips inside 40 polls to
// declare CYCLING and 60 flip-free polls to end it.
#include <unity.h>

#include <cmath>

#include "net/plug_cycle.h"

using plug::CycleDetector;
using plug::CycleEvent;
using plug::kCycleDefaults;

void setUp() {}
void tearDown() {}

constexpr float kSpeed10W = 30.2f;  // the baseline for the commanded speed
constexpr float kRunW = 45.0f;      // what the 08-20 fan drew when it ran (speed-12 level)
constexpr float kStopW = 4.3f;      // electronics only

// Feed `n` polls of one reading at a steady, settled speed 10.
static CycleEvent feed(CycleDetector& d, float w, int n, int speed = 10, float e = kSpeed10W) {
  CycleEvent last = CycleEvent::kNone;
  for (int i = 0; i < n; i++) {
    const CycleEvent ev = d.poll(speed, e, w, true);
    if (ev != CycleEvent::kNone)
      last = ev;
  }
  return last;
}

// ------------------------------------------------------------------ onset

static void test_quiet_fan_never_cycles() {
  CycleDetector d;
  for (int i = 0; i < 2000; i++) {
    TEST_ASSERT_EQUAL(static_cast<int>(CycleEvent::kNone),
                      static_cast<int>(d.poll(10, kSpeed10W, 29.0f + (i % 3), true)));
  }
  TEST_ASSERT_FALSE(d.cycling);
  TEST_ASSERT_EQUAL_UINT8(0, d.flips_in_window());
}

static void test_the_0820_night_is_called_cycling() {
  // Run a minute, stop two, run a minute, stop two... each class holds for
  // several polls, so every edge is a confirmed flip. Four edges in under
  // ten minutes is the profile.
  CycleDetector d;
  CycleEvent got = CycleEvent::kNone;
  for (int round = 0; round < 3 && got == CycleEvent::kNone; round++) {
    if (feed(d, kRunW, 4) == CycleEvent::kOnset)
      got = CycleEvent::kOnset;
    if (got == CycleEvent::kNone && feed(d, kStopW, 8) == CycleEvent::kOnset)
      got = CycleEvent::kOnset;
  }
  TEST_ASSERT_EQUAL(static_cast<int>(CycleEvent::kOnset), static_cast<int>(got));
  TEST_ASSERT_TRUE(d.cycling);
  TEST_ASSERT_TRUE(d.flips_in_window() >= 4);
}

static void test_one_stop_and_restart_is_a_hiccup_not_a_cycle() {
  CycleDetector d;
  feed(d, kRunW, 20);
  feed(d, kStopW, 8);                         // stops for two minutes
  const CycleEvent ev = feed(d, kRunW, 200);  // and runs on
  TEST_ASSERT_EQUAL(static_cast<int>(CycleEvent::kNone), static_cast<int>(ev));
  TEST_ASSERT_FALSE(d.cycling);
}

static void test_a_fan_that_dies_is_not_cycling() {
  // That is the plug verdict's DISAGREE case; this profile must stay quiet
  // so the two alerts never describe one event twice.
  CycleDetector d;
  feed(d, 30.0f, 40);
  TEST_ASSERT_EQUAL(static_cast<int>(CycleEvent::kNone), static_cast<int>(feed(d, kStopW, 400)));
  TEST_ASSERT_FALSE(d.cycling);
}

// ---------------------------------------------------------- meter hygiene

static void test_a_single_poll_blip_does_not_count() {
  // One 0 W reading between good ones is the meter (or HA) hiccuping. With
  // confirm_polls = 2 it flips nothing; forty of them in a row would have
  // declared cycling under a one-poll rule.
  CycleDetector d;
  feed(d, 30.0f, 10);
  for (int i = 0; i < 40; i++) {
    d.poll(10, kSpeed10W, 0.0f, true);
    feed(d, 30.0f, 3);
  }
  TEST_ASSERT_FALSE(d.cycling);
  TEST_ASSERT_EQUAL_UINT8(0, d.flips_in_window());
}

static void test_mid_band_readings_vote_for_nobody() {
  // A meter averaging across an edge reports something between stopped and
  // running. Those polls neither flip nor confirm; the register still ages.
  CycleDetector d;
  feed(d, kRunW, 4);
  feed(d, 15.0f, 100);  // 50 % of the baseline: inside the dead band
  TEST_ASSERT_FALSE(d.cycling);
  TEST_ASSERT_EQUAL_INT8(1, d.last_class);  // still believes it is running
}

static void test_missing_readings_and_unsettled_speed_are_skipped() {
  CycleDetector d;
  for (int i = 0; i < 100; i++) {
    d.poll(10, kSpeed10W, NAN, true);                        // HA unreachable
    d.poll(10, kSpeed10W, (i & 1) ? kRunW : kStopW, false);  // still settling
  }
  TEST_ASSERT_FALSE(d.cycling);
  TEST_ASSERT_EQUAL_UINT8(0, d.flips_in_window());
}

static void test_low_speeds_are_not_separable() {
  // Speed 1 expects 3.7 W; the fan's idle electronics alone are 1.4 W. No
  // meter can tell those apart, so the profile declines to rule.
  CycleDetector d;
  for (int i = 0; i < 200; i++) d.poll(1, 3.7f, (i & 1) ? 3.9f : 1.4f, true);
  TEST_ASSERT_FALSE(d.cycling);
  TEST_ASSERT_EQUAL_UINT8(0, d.flips_in_window());
}

// ------------------------------------------------------------ the episode

static CycleDetector cycling_detector() {
  CycleDetector d;
  for (int round = 0; round < 3; round++) {
    feed(d, kRunW, 4);
    feed(d, kStopW, 8);
  }
  return d;
}

static void test_a_speed_change_ends_the_episode_and_resets_the_profile() {
  CycleDetector d = cycling_detector();
  TEST_ASSERT_TRUE(d.cycling);
  const CycleEvent ev = d.poll(9, 25.0f, kRunW, false);
  TEST_ASSERT_EQUAL(static_cast<int>(CycleEvent::kEnded), static_cast<int>(ev));
  TEST_ASSERT_FALSE(d.cycling);
  TEST_ASSERT_EQUAL_UINT8(0, d.flips_in_window());
  TEST_ASSERT_EQUAL_INT8(0, d.last_class);
}

static void test_fifteen_quiet_minutes_end_it_and_ten_do_not() {
  CycleDetector d = cycling_detector();
  TEST_ASSERT_TRUE(d.cycling);
  // The fan stays stopped (a restart would itself be a flip). 40 quiet
  // polls: the onset window is empty but the episode holds.
  TEST_ASSERT_EQUAL(static_cast<int>(CycleEvent::kNone), static_cast<int>(feed(d, kStopW, 40)));
  TEST_ASSERT_TRUE(d.cycling);
  TEST_ASSERT_EQUAL_UINT8(0, d.flips_in_window());
  // 20 more (over 60 since the last flip = 15 min): ended.
  TEST_ASSERT_EQUAL(static_cast<int>(CycleEvent::kEnded), static_cast<int>(feed(d, kStopW, 20)));
  TEST_ASSERT_FALSE(d.cycling);
}

static void test_the_testimony_counts_the_episode() {
  CycleDetector d = cycling_detector();
  const uint16_t at_onset = d.polls;
  feed(d, kRunW, 4);
  feed(d, kStopW, 8);
  TEST_ASSERT_EQUAL_UINT16(at_onset + 12, d.polls);
  TEST_ASSERT_TRUE(d.run_polls >= 4);
  TEST_ASSERT_TRUE(d.stop_polls >= 8);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, kRunW, d.peak_w);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, kStopW, d.trough_w);
  TEST_ASSERT_TRUE(d.flips_total >= 6);
}

static void test_bucket_flips_count_since_last_take() {
  // The 5-minute history row reads this; it must hand over exactly the flips
  // since the previous row and then start again from zero.
  CycleDetector d;
  feed(d, kRunW, 4);
  feed(d, kStopW, 4);
  feed(d, kRunW, 4);
  TEST_ASSERT_EQUAL_UINT8(2, d.take_bucket_flips());
  TEST_ASSERT_EQUAL_UINT8(0, d.take_bucket_flips());
  feed(d, kStopW, 4);
  TEST_ASSERT_EQUAL_UINT8(1, d.take_bucket_flips());
}

static void test_register_aging_forgets_old_flips() {
  // Three flips, then 40 quiet polls: the window is empty again, so a later
  // lone flip cannot sum with ancient history into an onset.
  CycleDetector d;
  feed(d, kRunW, 4);
  feed(d, kStopW, 4);
  feed(d, kRunW, 4);
  feed(d, kStopW, 4);
  TEST_ASSERT_EQUAL_UINT8(3, d.flips_in_window());
  feed(d, kStopW, 40);
  TEST_ASSERT_EQUAL_UINT8(0, d.flips_in_window());
  feed(d, kRunW, 4);
  TEST_ASSERT_EQUAL_UINT8(1, d.flips_in_window());
  TEST_ASSERT_FALSE(d.cycling);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_quiet_fan_never_cycles);
  RUN_TEST(test_the_0820_night_is_called_cycling);
  RUN_TEST(test_one_stop_and_restart_is_a_hiccup_not_a_cycle);
  RUN_TEST(test_a_fan_that_dies_is_not_cycling);
  RUN_TEST(test_a_single_poll_blip_does_not_count);
  RUN_TEST(test_mid_band_readings_vote_for_nobody);
  RUN_TEST(test_missing_readings_and_unsettled_speed_are_skipped);
  RUN_TEST(test_low_speeds_are_not_separable);
  RUN_TEST(test_a_speed_change_ends_the_episode_and_resets_the_profile);
  RUN_TEST(test_fifteen_quiet_minutes_end_it_and_ten_do_not);
  RUN_TEST(test_the_testimony_counts_the_episode);
  RUN_TEST(test_bucket_flips_count_since_last_take);
  RUN_TEST(test_register_aging_forgets_old_flips);
  return UNITY_END();
}
