// Host tests for the daily-runtime accounting. See odometer_logic.h.
#include <unity.h>

#include "system/odometer_logic.h"

using odometer_logic::credit;

void setUp() {}
void tearDown() {}

static void idle_fan_credits_nothing() {
  const auto c = credit(60, /*running=*/false, true, 20260811, 20260811, 43200);
  TEST_ASSERT_EQUAL_UINT32(0, c.total_s);
  TEST_ASSERT_EQUAL_UINT32(0, c.today_s);
  TEST_ASSERT_FALSE(c.reset_today);
}

static void ordinary_tick_credits_both() {
  const auto c = credit(30, true, true, 20260811, 20260811, 43200);
  TEST_ASSERT_EQUAL_UINT32(30, c.total_s);
  TEST_ASSERT_EQUAL_UINT32(30, c.today_s);
  TEST_ASSERT_FALSE(c.reset_today);
}

// The bug: a tick spanning local midnight reset the daily counter AFTER
// crediting, so the seconds belonging to the new day were zeroed too.
static void midnight_span_keeps_the_new_days_share() {
  // 300 s tick, 120 s of which is after local midnight.
  const auto c = credit(300, true, true, 20260812, 20260811, 120);
  TEST_ASSERT_TRUE(c.reset_today);
  TEST_ASSERT_EQUAL_UINT32(300, c.total_s);  // lifetime keeps all of it
  TEST_ASSERT_EQUAL_UINT32(120, c.today_s);  // today keeps only its own share
}

// A stalled loop can produce a span longer than the new day so far.
static void span_longer_than_the_new_day_is_capped() {
  const auto c = credit(600, true, true, 20260812, 20260811, 90);
  TEST_ASSERT_TRUE(c.reset_today);
  TEST_ASSERT_EQUAL_UINT32(90, c.today_s);
}

// Exactly on the boundary: nothing of this span belongs to the new day.
static void midnight_exactly_credits_zero_today() {
  const auto c = credit(300, true, true, 20260812, 20260811, 0);
  TEST_ASSERT_TRUE(c.reset_today);
  TEST_ASSERT_EQUAL_UINT32(0, c.today_s);
  TEST_ASSERT_EQUAL_UINT32(300, c.total_s);
}

// First run ever: prev_ymd == 0 must not look like a rollover and wipe the
// counter restored from NVS.
static void first_tick_after_boot_does_not_reset() {
  const auto c = credit(45, true, true, 20260811, 0, 43200);
  TEST_ASSERT_FALSE(c.reset_today);
  TEST_ASSERT_EQUAL_UINT32(45, c.today_s);
}

// Before SNTP there is no calendar to reason about; credit normally rather
// than guessing at a rollover.
static void unsynced_clock_credits_without_rolling() {
  const auto c = credit(45, true, /*clock_valid=*/false, 0, 0, 0);
  TEST_ASSERT_FALSE(c.reset_today);
  TEST_ASSERT_EQUAL_UINT32(45, c.total_s);
  TEST_ASSERT_EQUAL_UINT32(45, c.today_s);
}

// The counter must survive a same-day tick after a long stall.
static void long_same_day_stall_credits_in_full() {
  const auto c = credit(3600, true, true, 20260811, 20260811, 50000);
  TEST_ASSERT_EQUAL_UINT32(3600, c.today_s);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(idle_fan_credits_nothing);
  RUN_TEST(ordinary_tick_credits_both);
  RUN_TEST(midnight_span_keeps_the_new_days_share);
  RUN_TEST(span_longer_than_the_new_day_is_capped);
  RUN_TEST(midnight_exactly_credits_zero_today);
  RUN_TEST(first_tick_after_boot_does_not_reset);
  RUN_TEST(unsynced_clock_credits_without_rolling);
  RUN_TEST(long_same_day_stall_credits_in_full);
  return UNITY_END();
}
