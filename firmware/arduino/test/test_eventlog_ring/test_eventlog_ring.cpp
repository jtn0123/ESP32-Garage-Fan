// Copyright 2026 Justin
// Native tests for system/eventlog_ring.h -- the flight recorder's RAM ring.
// Small template sizes so wrap and ledger overflow are cheap to reach.
#include <unity.h>

#include <cstring>

#include "system/eventlog_ring.h"

using eventlog::LineRing;

// Unity requires both lifecycle hooks; these tests keep no fixture state,
// so there is nothing to set up or release between cases.
void setUp() {}
void tearDown() {}

static void test_appends_read_back_oldest_first() {
  LineRing<4, 16> r;
  r.append("one");
  r.append("two");
  r.append("three");
  TEST_ASSERT_EQUAL_UINT16(3, r.n);
  TEST_ASSERT_EQUAL_STRING("one", r.line(0));
  TEST_ASSERT_EQUAL_STRING("three", r.line(2));
}

static void test_wrap_overwrites_oldest() {
  LineRing<3, 16> r;
  r.append("a");
  r.append("b");
  r.append("c");
  r.append("d");
  r.append("e");
  TEST_ASSERT_EQUAL_UINT16(3, r.n);
  TEST_ASSERT_EQUAL_STRING("c", r.line(0));
  TEST_ASSERT_EQUAL_STRING("e", r.line(2));
}

static void test_long_line_truncates_with_nul() {
  LineRing<2, 8> r;
  r.append("0123456789abcdef");
  TEST_ASSERT_EQUAL_STRING("0123456", r.line(0));  // 7 chars + NUL
}

static void test_flush_ledger_counts_down() {
  LineRing<4, 16> r;
  r.append("a");
  r.append("b");
  r.append("c");
  TEST_ASSERT_EQUAL_UINT16(3, r.unflushed());
  TEST_ASSERT_EQUAL_UINT16(0, r.first_unflushed());
  r.mark_flushed(2);
  TEST_ASSERT_EQUAL_UINT16(1, r.unflushed());
  TEST_ASSERT_EQUAL_STRING("c", r.line(r.first_unflushed()));
  r.mark_flushed(1);
  TEST_ASSERT_EQUAL_UINT16(0, r.unflushed());
}

static void test_overwriting_unflushed_lines_counts_as_lost() {
  // No SD for a long stretch: the ring wraps past lines that never flushed.
  // The ledger must advance past them (they are gone) and record the loss,
  // and unflushed() must never exceed what the ring still holds.
  LineRing<3, 16> r;
  for (int i = 0; i < 7; i++) r.append("x");
  TEST_ASSERT_EQUAL_UINT16(3, r.unflushed());
  TEST_ASSERT_EQUAL_UINT32(4, r.lost);
  TEST_ASSERT_EQUAL_UINT16(0, r.first_unflushed());
}

static void test_flush_then_wrap_then_flush() {
  LineRing<3, 16> r;
  r.append("a");
  r.append("b");
  r.mark_flushed(2);  // both on SD
  r.append("c");
  r.append("d");  // ring now b,c,d; only c,d owe a write
  TEST_ASSERT_EQUAL_UINT16(2, r.unflushed());
  TEST_ASSERT_EQUAL_STRING("c", r.line(r.first_unflushed()));
  TEST_ASSERT_EQUAL_UINT32(0, r.lost);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_appends_read_back_oldest_first);
  RUN_TEST(test_wrap_overwrites_oldest);
  RUN_TEST(test_long_line_truncates_with_nul);
  RUN_TEST(test_flush_ledger_counts_down);
  RUN_TEST(test_overwriting_unflushed_lines_counts_as_lost);
  RUN_TEST(test_flush_then_wrap_then_flush);
  return UNITY_END();
}
