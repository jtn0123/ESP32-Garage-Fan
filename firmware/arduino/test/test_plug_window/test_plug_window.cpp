// Copyright 2026 Justin
// Native tests for net/plug_window.h -- the meter window behind the 5-minute
// `plug window ...` telemetry line, the raw-sample trace ring, and the
// implied-speed lookup that names what a measured draw really is.
#include <unity.h>

#include <cmath>
#include <cstring>

#include "net/plug_window.h"

using plug::Reading;
using plug::Trace;
using plug::Window;

void setUp() {}
void tearDown() {}

// The measured baseline (net/plug.cpp), speeds 0..12.
static const float kBase[13] = {1.4f,  3.7f,  4.8f,  7.0f,  7.6f,  10.0f, 12.5f,
                                15.8f, 19.7f, 25.0f, 30.2f, 37.7f, 44.2f};

// ------------------------------------------------------------ implied speed

static void the_night_of_0820_reads_as_speed_twelve() {
  // The whole point: the controller held speed 10 (30.2 W) and the meter kept
  // reading ~45 W. Nothing on the tape ever said that is the SPEED-12 level.
  TEST_ASSERT_EQUAL_INT(12, plug::nearest_speed(kBase, 13, 45.3f));
  TEST_ASSERT_EQUAL_INT(10, plug::nearest_speed(kBase, 13, 30.4f));
  TEST_ASSERT_EQUAL_INT(0, plug::nearest_speed(kBase, 13, 1.5f));
}

static void an_absent_reading_implies_nothing() {
  TEST_ASSERT_EQUAL_INT(-1, plug::nearest_speed(kBase, 13, NAN));
  TEST_ASSERT_EQUAL_INT(-1, plug::nearest_speed(nullptr, 13, 20.0f));
  TEST_ASSERT_EQUAL_INT(-1, plug::nearest_speed(kBase, 0, 20.0f));
}

// ------------------------------------------------------------------- window

static void it_folds_a_window_down_to_range_and_counts() {
  Window w;
  w.feed(4.3f, -1);
  w.feed(45.3f, 1);
  w.feed(4.3f, -1);
  TEST_ASSERT_EQUAL_UINT16(3, w.polls);
  TEST_ASSERT_EQUAL_UINT16(1, w.run);
  TEST_ASSERT_EQUAL_UINT16(2, w.stop);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 4.3f, w.min_w);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 45.3f, w.max_w);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, (4.3f + 45.3f + 4.3f) / 3, w.mean());
}

static void a_missed_poll_counts_without_polluting_the_range() {
  Window w;
  w.feed(20.0f, 1);
  w.feed(NAN, 0);  // HA unreachable
  w.feed(20.0f, 1);
  TEST_ASSERT_EQUAL_UINT16(3, w.polls);
  TEST_ASSERT_EQUAL_UINT16(1, w.missed);
  TEST_ASSERT_EQUAL_UINT16(2, w.n_w);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, w.min_w);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, w.max_w);
}

static void changed_counts_the_meters_own_update_rate() {
  // The metric that measures the METER, not the fan: HA serves the same value
  // until its sensor updates, so a window where few polls carry a new number
  // says this device is oversampling a slow sensor -- the blind spot that
  // lets a fast cycle read as a steady mid-band draw.
  Window w;
  for (int i = 0; i < 4; i++) w.feed(20.0f, 1);
  w.feed(21.0f, 1);
  for (int i = 0; i < 4; i++) w.feed(21.0f, 1);
  TEST_ASSERT_EQUAL_UINT16(2, w.changed);  // the first reading, then the step
}

static void reset_clears_the_counters_but_remembers_the_last_reading() {
  // Otherwise the first poll of every window counts as "changed" and the
  // update-rate metric reads five minutes too optimistic, forever.
  Window w;
  w.feed(20.0f, 1);
  w.feed(20.0f, 1);
  w.add_flips(3);
  w.reset();
  TEST_ASSERT_EQUAL_UINT16(0, w.polls);
  TEST_ASSERT_EQUAL_UINT16(0, w.flips);
  TEST_ASSERT_EQUAL_UINT16(0, w.n_w);
  w.feed(20.0f, 1);
  TEST_ASSERT_EQUAL_UINT16(0, w.changed);  // same value as before the reset
  w.feed(25.0f, 1);
  TEST_ASSERT_EQUAL_UINT16(1, w.changed);
}

static void an_empty_window_means_nan_not_zero() {
  Window w;
  TEST_ASSERT_TRUE(std::isnan(w.mean()));
}

// ---------------------------------------------------------------- the trace

static void the_trace_keeps_the_newest_samples_oldest_first() {
  Trace<4> t;
  for (int i = 0; i < 6; i++) t.push(plug::encode_reading(10.0f + i, 9, 1));
  TEST_ASSERT_EQUAL_UINT8(4, t.size());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 12.0f, t.at(0).watts());  // 10 and 11 evicted
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 15.0f, t.at(3).watts());
}

static void a_reading_round_trips_through_the_encoding() {
  const Reading r = plug::encode_reading(45.34f, 10, -1);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 45.3f, r.watts());
  TEST_ASSERT_EQUAL_INT8(10, r.speed);
  TEST_ASSERT_EQUAL_INT8(-1, r.cls);
}

static void a_missing_reading_survives_as_missing() {
  const Reading r = plug::encode_reading(NAN, 10, 0);
  TEST_ASSERT_EQUAL_UINT16(Reading::kNoRead, r.dw);
  TEST_ASSERT_TRUE(std::isnan(r.watts()));
}

static void an_absurd_reading_saturates_rather_than_wrapping() {
  // A corrupt HA response must not come back as a plausible small number.
  const Reading r = plug::encode_reading(1e9f, 10, 1);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 6553.4f, r.watts());
}

// ----------------------------------------------------------------- the line

static void the_line_says_commanded_and_implied_side_by_side() {
  Window w;
  for (int i = 0; i < 8; i++) w.feed(45.3f, 1);
  for (int i = 0; i < 28; i++) w.feed(4.3f, -1);
  w.add_flips(5);
  char out[plug::kLogMsgCap];
  plug::format_window_line(out, sizeof(out), w, 10, 84, 12);
  TEST_ASSERT_NOT_NULL(strstr(out, "sp10/~12"));
  TEST_ASSERT_NOT_NULL(strstr(out, "pad=84%"));
  TEST_ASSERT_NOT_NULL(strstr(out, "w=4.3/"));
  TEST_ASSERT_NOT_NULL(strstr(out, "/45.3"));
  TEST_ASSERT_NOT_NULL(strstr(out, "on=8"));
  TEST_ASSERT_NOT_NULL(strstr(out, "off=28"));
  TEST_ASSERT_NOT_NULL(strstr(out, "cyc=5"));
}

static void a_window_with_no_reads_says_so_instead_of_printing_zeroes() {
  Window w;
  for (int i = 0; i < 20; i++) w.feed(NAN, 0);
  char out[plug::kLogMsgCap];
  plug::format_window_line(out, sizeof(out), w, 10, 84, -1);
  TEST_ASSERT_NOT_NULL(strstr(out, "no meter reads"));
  TEST_ASSERT_NULL(strstr(out, "w=0.0"));
}

static void the_worst_case_line_fits_the_recorders_budget() {
  // A truncated line loses its TAIL, which is where the counts are -- and it
  // does so silently. Feed the formatter values no real window can produce
  // and pin that it still fits.
  Window w;
  w.polls = w.run = w.stop = w.changed = w.missed = w.flips = 65535;
  w.n_w = 65535;
  w.min_w = w.max_w = 1e9f;
  w.sum_w = 1e9f;
  char out[256];
  const int n = plug::format_window_line(out, sizeof(out), w, -1, 100, -1);
  TEST_ASSERT_TRUE_MESSAGE(n > 0 && static_cast<size_t>(n) < plug::kLogMsgCap,
                           "the window line must fit eventlog's 80-byte message");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(the_night_of_0820_reads_as_speed_twelve);
  RUN_TEST(an_absent_reading_implies_nothing);
  RUN_TEST(it_folds_a_window_down_to_range_and_counts);
  RUN_TEST(a_missed_poll_counts_without_polluting_the_range);
  RUN_TEST(changed_counts_the_meters_own_update_rate);
  RUN_TEST(reset_clears_the_counters_but_remembers_the_last_reading);
  RUN_TEST(an_empty_window_means_nan_not_zero);
  RUN_TEST(the_trace_keeps_the_newest_samples_oldest_first);
  RUN_TEST(a_reading_round_trips_through_the_encoding);
  RUN_TEST(a_missing_reading_survives_as_missing);
  RUN_TEST(an_absurd_reading_saturates_rather_than_wrapping);
  RUN_TEST(the_line_says_commanded_and_implied_side_by_side);
  RUN_TEST(a_window_with_no_reads_says_so_instead_of_printing_zeroes);
  RUN_TEST(the_worst_case_line_fits_the_recorders_budget);
  return UNITY_END();
}
