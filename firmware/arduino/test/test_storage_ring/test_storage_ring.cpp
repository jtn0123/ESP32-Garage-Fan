// Copyright 2026 Justin
// Native tests for storage/ring_logic.h -- the 24 h sample ring. A Ring<4>
// stands in for the device's Ring<288> so eviction is cheap to reach; the
// template is the same code either way.
#include <unity.h>

#include <cmath>

#include "storage/ring_logic.h"

using history::Ring;
using history::Sample;

// Unity requires both lifecycle hooks; these tests keep no fixture state,
// so there is nothing to set up or release between cases.
void setUp() {}
void tearDown() {}

static Sample row(float t, int8_t speed = 3) {
  Sample s;
  s.t = t;
  s.h = t + 40;  // distinct per-column values so a copy/evict mix-up shows
  s.p = t + 900;
  s.out_f = t + 60;
  s.batt_v = 3.7f;
  s.speed = speed;
  s.chg = 1;
  s.flips = (int8_t)(speed + 1);  // distinct again, for the column-moves test
  s.w_min = t - 5;
  s.w_max = t + 5;
  return s;
}

static void test_fills_in_order() {
  Ring<4> r;
  TEST_ASSERT_EQUAL_UINT16(0, r.n);
  r.append(row(20));
  r.append(row(21));
  r.append(row(22));
  TEST_ASSERT_EQUAL_UINT16(3, r.n);
  TEST_ASSERT_EQUAL_FLOAT(20, r.t[0]);
  TEST_ASSERT_EQUAL_FLOAT(22, r.t[2]);
}

static void test_evicts_oldest_once_full() {
  Ring<4> r;
  for (int i = 0; i < 6; i++) r.append(row(20 + i, (int8_t)i));
  TEST_ASSERT_EQUAL_UINT16(4, r.n);  // never exceeds capacity
  TEST_ASSERT_EQUAL_FLOAT(22, r.t[0]);
  TEST_ASSERT_EQUAL_FLOAT(25, r.t[3]);  // newest at the end
}

static void test_all_columns_move_together() {
  // The eviction is seven separate memmoves; a missed column would silently
  // pair sample i's temperature with sample i+1's speed forever after.
  Ring<3> r;
  for (int i = 0; i < 5; i++) r.append(row(10 + i, (int8_t)(i)));
  for (uint16_t i = 0; i < r.n; i++) {
    TEST_ASSERT_EQUAL_FLOAT(r.t[i] + 40, r.h[i]);
    TEST_ASSERT_EQUAL_FLOAT(r.t[i] + 900, r.p[i]);
    TEST_ASSERT_EQUAL_FLOAT(r.t[i] + 60, r.o[i]);
    TEST_ASSERT_EQUAL_INT8((int8_t)(r.t[i] - 10), r.s[i]);
    TEST_ASSERT_EQUAL_INT8((int8_t)(r.s[i] + 1), r.f[i]);
    TEST_ASSERT_EQUAL_FLOAT(r.t[i] - 5, r.wn[i]);
    TEST_ASSERT_EQUAL_FLOAT(r.t[i] + 5, r.wx[i]);
  }
}

static void test_stats_empty_ring_is_all_nan() {
  Ring<4> r;
  float mn, mx, avg;
  r.temp_stats(&mn, &mx, &avg);
  TEST_ASSERT_TRUE(std::isnan(mn));
  TEST_ASSERT_TRUE(std::isnan(mx));
  TEST_ASSERT_TRUE(std::isnan(avg));
}

static void test_stats_single_sample() {
  Ring<4> r;
  r.append(row(23.5f));
  float mn, mx, avg;
  r.temp_stats(&mn, &mx, &avg);
  TEST_ASSERT_EQUAL_FLOAT(23.5f, mn);
  TEST_ASSERT_EQUAL_FLOAT(23.5f, mx);
  TEST_ASSERT_EQUAL_FLOAT(23.5f, avg);
}

static void test_stats_cover_only_whats_retained() {
  // After eviction the dropped rows must stop influencing min/max/avg.
  Ring<3> r;
  r.append(row(99));  // will be evicted
  r.append(row(10));
  r.append(row(20));
  r.append(row(30));
  float mn, mx, avg;
  r.temp_stats(&mn, &mx, &avg);
  TEST_ASSERT_EQUAL_FLOAT(10, mn);
  TEST_ASSERT_EQUAL_FLOAT(30, mx);
  TEST_ASSERT_EQUAL_FLOAT(20, avg);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fills_in_order);
  RUN_TEST(test_evicts_oldest_once_full);
  RUN_TEST(test_all_columns_move_together);
  RUN_TEST(test_stats_empty_ring_is_all_nan);
  RUN_TEST(test_stats_single_sample);
  RUN_TEST(test_stats_cover_only_whats_retained);
  return UNITY_END();
}
