// Copyright 2026 Justin
// Native tests for system/fixed_fmt.h -- rendering a measurement into a log
// line without letting an absurd value eat the line.
#include <unity.h>

#include <cmath>
#include <cstring>

#include "system/fixed_fmt.h"

void setUp() {}
void tearDown() {}

static void it_renders_an_ordinary_value() {
  char out[fixedfmt::kCap];
  fixedfmt::write(out, fixedfmt::tenths(45.34f));
  TEST_ASSERT_EQUAL_STRING("45.3", out);
}

static void it_rounds_rather_than_truncating() {
  TEST_ASSERT_EQUAL_INT32(453, fixedfmt::tenths(45.25f));
  TEST_ASSERT_EQUAL_INT32(454, fixedfmt::tenths(45.44f));
}

static void a_small_negative_keeps_its_sign_and_its_zero() {
  // The bug this exists to prevent: "%ld.%ld" of (-5/10, -5%10) renders
  // "0.-5", which is not a number at all.
  char out[fixedfmt::kCap];
  fixedfmt::write(out, fixedfmt::tenths(-0.5f));
  TEST_ASSERT_EQUAL_STRING("-0.5", out);
}

static void absent_reads_as_absent_not_as_zero() {
  char out[fixedfmt::kCap];
  fixedfmt::write(out, fixedfmt::tenths(NAN));
  TEST_ASSERT_EQUAL_STRING("--", out);
  TEST_ASSERT_EQUAL_INT32(fixedfmt::kAbsent, fixedfmt::tenths_f(NAN));
}

static void an_absurd_value_costs_its_own_field_and_no_more() {
  // "%.1f" of 1e38 is 39 digits before the point. A line carrying that loses
  // its tail -- and the tail is where the counts are.
  char out[fixedfmt::kCap];
  fixedfmt::write(out, fixedfmt::tenths(1e38f));
  TEST_ASSERT_EQUAL_STRING("999.9", out);
  fixedfmt::write(out, fixedfmt::tenths(-1e38f));
  TEST_ASSERT_EQUAL_STRING("-999.9", out);
  // Every rendering fits the buffer, terminator included.
  TEST_ASSERT_TRUE(strlen(out) < fixedfmt::kCap);
}

static void an_infinity_clamps_to_the_top_of_the_range_on_every_platform() {
  // lround() of an infinity is UNDEFINED and the platforms disagree: glibc
  // returns LONG_MIN, so a value clamped only on the integer side rendered
  // the largest possible reading as "-999.9". Caught by CI, not by the Mac
  // this was written on.
  char out[fixedfmt::kCap];
  fixedfmt::write(out, fixedfmt::tenths(INFINITY));
  TEST_ASSERT_EQUAL_STRING("999.9", out);
  fixedfmt::write(out, fixedfmt::tenths(-INFINITY));
  TEST_ASSERT_EQUAL_STRING("-999.9", out);
  // And the overflow that PRODUCES an infinity inside the function: 1e38
  // is finite, 1e38 * 10 is not.
  TEST_ASSERT_EQUAL_INT32(9999, fixedfmt::tenths(1e38f));
  TEST_ASSERT_EQUAL_INT32(-9999, fixedfmt::tenths(-1e38f));
  TEST_ASSERT_EQUAL_INT32(9999, fixedfmt::tenths_f(3e38f));
}

static void a_caller_supplied_out_of_range_value_is_clamped_too() {
  // write() re-clamps rather than trusting its input: that is what lets the
  // compiler prove the field is at most six characters.
  char out[fixedfmt::kCap];
  fixedfmt::write(out, 123456);
  TEST_ASSERT_EQUAL_STRING("999.9", out);
}

static void celsius_becomes_fahrenheit_tenths() {
  char out[fixedfmt::kCap];
  fixedfmt::write(out, fixedfmt::tenths_f(27.2f));  // 81.0 F
  TEST_ASSERT_EQUAL_STRING("81.0", out);
}

static void a_differential_carries_an_explicit_sign() {
  char out[fixedfmt::kCap];
  fixedfmt::write_signed(out, fixedfmt::tenths(9.4f));
  TEST_ASSERT_EQUAL_STRING("+9.4", out);
  fixedfmt::write_signed(out, fixedfmt::tenths(-9.4f));
  TEST_ASSERT_EQUAL_STRING("-9.4", out);
  fixedfmt::write_signed(out, fixedfmt::kAbsent);
  TEST_ASSERT_EQUAL_STRING("--", out);
}

static void counts_clamp_to_their_field_width() {
  TEST_ASSERT_EQUAL_INT(20, fixedfmt::count99(20));
  TEST_ASSERT_EQUAL_INT(99, fixedfmt::count99(65535));
  TEST_ASSERT_EQUAL_INT(999, fixedfmt::count999(65535));
  TEST_ASSERT_EQUAL_INT(12, fixedfmt::small(12));
  TEST_ASSERT_EQUAL_INT(-9, fixedfmt::small(-1000));
  TEST_ASSERT_EQUAL_INT(99, fixedfmt::small(1000));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(it_renders_an_ordinary_value);
  RUN_TEST(it_rounds_rather_than_truncating);
  RUN_TEST(a_small_negative_keeps_its_sign_and_its_zero);
  RUN_TEST(absent_reads_as_absent_not_as_zero);
  RUN_TEST(an_absurd_value_costs_its_own_field_and_no_more);
  RUN_TEST(an_infinity_clamps_to_the_top_of_the_range_on_every_platform);
  RUN_TEST(a_caller_supplied_out_of_range_value_is_clamped_too);
  RUN_TEST(celsius_becomes_fahrenheit_tenths);
  RUN_TEST(a_differential_carries_an_explicit_sign);
  RUN_TEST(counts_clamp_to_their_field_width);
  return UNITY_END();
}
