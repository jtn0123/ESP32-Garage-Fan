// Unit tests for history_ring.h — the 24h sample ring behind the sparkline
// page. Pure header, so these compile natively with no Arduino shim.
#include <unity.h>
#include <cmath>

#include "../../src/history_ring.h"

static void reset_ring() {
  HistoryRing& h = hist_ring();
  h.count = 0;
  h.head = 0;
}

void test_push_and_oldest_first_ordering() {
  reset_ring();
  hist_push(70.0f, 60.0f, 30.0f, 50.0f);
  hist_push(71.0f, 61.0f, 31.0f, 51.0f);
  hist_push(72.0f, 62.0f, 32.0f, 52.0f);
  TEST_ASSERT_EQUAL_UINT16(3, hist_ring().count);
  TEST_ASSERT_EQUAL_FLOAT(70.0f, hist_at(hist_ring().temp_in, 0));
  TEST_ASSERT_EQUAL_FLOAT(72.0f, hist_at(hist_ring().temp_in, 2));
  TEST_ASSERT_EQUAL_FLOAT(62.0f, hist_at(hist_ring().temp_out, 2));
}

void test_wraparound_keeps_newest_cap_samples() {
  reset_ring();
  for (int i = 0; i < HIST_CAP + 10; i++) {
    hist_push(static_cast<float>(i), NAN, 0.0f, NAN);
  }
  TEST_ASSERT_EQUAL_UINT16(HIST_CAP, hist_ring().count);
  // Oldest retained should be sample #10, newest #(CAP+9).
  TEST_ASSERT_EQUAL_FLOAT(10.0f, hist_at(hist_ring().temp_in, 0));
  TEST_ASSERT_EQUAL_FLOAT(static_cast<float>(HIST_CAP + 9),
                          hist_at(hist_ring().temp_in, HIST_CAP - 1));
}

void test_minmax_shared_across_pair_and_skips_nan() {
  reset_ring();
  hist_push(70.0f, 55.0f, 30.0f, NAN);
  hist_push(NAN, 88.0f, 35.0f, NAN);
  hist_push(73.0f, NAN, 28.0f, NAN);
  float mn = 0, mx = 0;
  TEST_ASSERT_TRUE(hist_minmax(hist_ring().temp_in, hist_ring().temp_out, &mn, &mx));
  TEST_ASSERT_EQUAL_FLOAT(55.0f, mn);  // outdoor low
  TEST_ASSERT_EQUAL_FLOAT(88.0f, mx);  // outdoor high
  // RH pair: outdoor all-NaN must not poison the result.
  TEST_ASSERT_TRUE(hist_minmax(hist_ring().rh_in, hist_ring().rh_out, &mn, &mx));
  TEST_ASSERT_EQUAL_FLOAT(28.0f, mn);
  TEST_ASSERT_EQUAL_FLOAT(35.0f, mx);
}

void test_minmax_empty_and_all_nan_returns_false() {
  reset_ring();
  float mn = 0, mx = 0;
  TEST_ASSERT_FALSE(hist_minmax(hist_ring().temp_in, hist_ring().temp_out, &mn, &mx));
  hist_push(NAN, NAN, NAN, NAN);
  TEST_ASSERT_FALSE(hist_minmax(hist_ring().temp_in, hist_ring().temp_out, &mn, &mx));
}

void test_minmax_flat_series_gets_nonzero_span() {
  reset_ring();
  hist_push(72.0f, NAN, 40.0f, NAN);
  hist_push(72.0f, NAN, 40.0f, NAN);
  float mn = 0, mx = 0;
  TEST_ASSERT_TRUE(hist_minmax(hist_ring().temp_in, nullptr, &mn, &mx));
  // A flat line must not produce a zero span (renderers divide by mx-mn).
  TEST_ASSERT_TRUE(mx > mn);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_push_and_oldest_first_ordering);
  RUN_TEST(test_wraparound_keeps_newest_cap_samples);
  RUN_TEST(test_minmax_shared_across_pair_and_skips_nan);
  RUN_TEST(test_minmax_empty_and_all_nan_returns_false);
  RUN_TEST(test_minmax_flat_series_gets_nonzero_span);
  return UNITY_END();
}
