// Copyright 2026 Justin
// Native tests for sensors/outdoor.h -- the single-source outdoor reading
// that replaced the open-meteo/Home-Assistant race in 1.23.0.
#include <unity.h>

#include <cmath>

#include "sensors/outdoor.h"

namespace {

constexpr uint32_t kStale = 30u * 60u * 1000u;  // kOutdoorStaleMs
constexpr uint32_t kPoll = 10u * 60u * 1000u;   // weather.cpp's cadence
using Feed = sensors::OutdoorFeed<3, kStale>;

}  // namespace

void setUp() {}
void tearDown() {}

static void test_nothing_yet_is_not_a_reading() {
  Feed f;
  TEST_ASSERT_TRUE(f.stale(0));
  TEST_ASSERT_TRUE(std::isnan(f.value_c(0)));
  // millis() == 0 is a legal timestamp, so "never" cannot be spelled last_ms=0.
  TEST_ASSERT_TRUE(std::isnan(f.value_c(1)));
}

static void test_answers_from_the_first_poll() {
  // A fresh boot must have an opinion immediately, not wait 30 min to fill
  // the window -- same contract as RollingAvg.
  Feed f;
  f.push(20.0f, 1000);
  TEST_ASSERT_FALSE(f.stale(1000));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, f.value_c(1000));
}

static void test_mean_spreads_a_step_over_the_window() {
  // The whole point: open-meteo steps >= 1.0 degF on 22% of polls, wider than
  // the 1 degF band. Three polls of coverage turn one step into three.
  Feed f;
  uint32_t t = 0;
  f.push(20.0f, t);
  f.push(20.0f, t += kPoll);
  f.push(20.0f, t += kPoll);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, f.value_c(t));
  f.push(23.0f, t += kPoll);                              // a 3 C jump arrives
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f, f.value_c(t));  // seen as 1 C
  f.push(23.0f, t += kPoll);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 22.0f, f.value_c(t));
  f.push(23.0f, t += kPoll);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 23.0f, f.value_c(t));  // fully arrived
}

static void test_expires_and_holds_nothing_after() {
  Feed f;
  f.push(20.0f, 1000);
  TEST_ASSERT_FALSE(f.stale(1000 + kStale));     // exactly at the deadline
  TEST_ASSERT_TRUE(f.stale(1000 + kStale + 1));  // one ms past it
  TEST_ASSERT_TRUE(std::isnan(f.value_c(1000 + kStale + 1)));
}

static void test_gap_longer_than_the_expiry_clears_the_window() {
  // Yesterday's weather must not blend into the first reading back.
  Feed f;
  uint32_t t = 0;
  f.push(30.0f, t);
  f.push(30.0f, t += kPoll);
  const uint32_t after_outage = t + kStale + kPoll;
  f.push(10.0f, after_outage);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f, f.value_c(after_outage));
}

static void test_short_gap_keeps_the_window() {
  // A single missed poll is a hiccup, not an outage: the mean must coast.
  Feed f;
  uint32_t t = 0;
  f.push(20.0f, t);
  f.push(20.0f, t += kPoll);
  f.push(26.0f, t += 2 * kPoll);  // one poll skipped, still inside kStale
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 22.0f, f.value_c(t));
}

static void test_failed_fetch_pushes_nothing() {
  // weather.cpp returns NAN on any failure; that must not poison the mean,
  // and it must not refresh the expiry either -- a feed that only fails is
  // a dead feed.
  Feed f;
  f.push(20.0f, 1000);
  f.push(NAN, 1000 + kPoll);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, f.value_c(1000 + kPoll));
  TEST_ASSERT_TRUE(f.stale(1000 + kStale + 1));
}

static void test_reset_forgets_everything() {
  Feed f;
  f.push(20.0f, 1000);
  f.reset();
  TEST_ASSERT_TRUE(f.stale(1000));
  TEST_ASSERT_TRUE(std::isnan(f.value_c(1000)));
}

static void test_raw_is_the_last_poll_not_the_mean() {
  Feed f;
  uint32_t t = 0;
  f.push(20.0f, t);
  f.push(26.0f, t += kPoll);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 23.0f, f.value_c(t));  // control sees this
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 26.0f, f.raw_c(t));    // telemetry sees this
  TEST_ASSERT_TRUE(std::isnan(f.raw_c(t + kStale + 1)));  // and it expires too
}

static void test_millis_rollover_does_not_resurrect_a_dead_feed() {
  // millis() wraps every 49.7 days; unsigned subtraction has to carry it.
  Feed f;
  const uint32_t before_wrap = 0xFFFFFFFFu - 1000u;
  f.push(20.0f, before_wrap);
  TEST_ASSERT_FALSE(f.stale(before_wrap + 1000u));          // wrapped to ~0
  TEST_ASSERT_TRUE(f.stale(before_wrap + kStale + 1000u));  // and still ages
}

// ------------------------------------------------------- BlindEdge

static void test_booting_healthy_is_not_news() {
  sensors::BlindEdge e;
  TEST_ASSERT_EQUAL(sensors::BlindEdge::kNone, e.update(false));
  TEST_ASSERT_EQUAL(sensors::BlindEdge::kNone, e.update(false));
}

static void test_booting_blind_says_so_immediately() {
  // A board that comes up with no weather at all must announce it, not wait
  // for a transition that already happened before it was watching.
  sensors::BlindEdge e;
  TEST_ASSERT_EQUAL(sensors::BlindEdge::kWentBlind, e.update(true));
}

static void test_one_line_each_way_never_per_poll() {
  sensors::BlindEdge e;
  e.update(false);
  TEST_ASSERT_EQUAL(sensors::BlindEdge::kWentBlind, e.update(true));
  for (int i = 0; i < 20; i++)
    TEST_ASSERT_EQUAL(sensors::BlindEdge::kNone, e.update(true));  // still dark, still quiet
  TEST_ASSERT_EQUAL(sensors::BlindEdge::kRestored, e.update(false));
  for (int i = 0; i < 20; i++) TEST_ASSERT_EQUAL(sensors::BlindEdge::kNone, e.update(false));
}

static void test_flapping_reports_every_real_edge() {
  sensors::BlindEdge e;
  e.update(false);
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT_EQUAL(sensors::BlindEdge::kWentBlind, e.update(true));
    TEST_ASSERT_EQUAL(sensors::BlindEdge::kRestored, e.update(false));
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_nothing_yet_is_not_a_reading);
  RUN_TEST(test_answers_from_the_first_poll);
  RUN_TEST(test_mean_spreads_a_step_over_the_window);
  RUN_TEST(test_expires_and_holds_nothing_after);
  RUN_TEST(test_gap_longer_than_the_expiry_clears_the_window);
  RUN_TEST(test_short_gap_keeps_the_window);
  RUN_TEST(test_failed_fetch_pushes_nothing);
  RUN_TEST(test_reset_forgets_everything);
  RUN_TEST(test_raw_is_the_last_poll_not_the_mean);
  RUN_TEST(test_millis_rollover_does_not_resurrect_a_dead_feed);
  RUN_TEST(test_booting_healthy_is_not_news);
  RUN_TEST(test_booting_blind_says_so_immediately);
  RUN_TEST(test_one_line_each_way_never_per_poll);
  RUN_TEST(test_flapping_reports_every_real_edge);
  return UNITY_END();
}
