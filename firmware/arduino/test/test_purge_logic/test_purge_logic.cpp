// Host tests for the SD purge bookkeeping. See purge_logic.h.
//
// The purge panicked the deployed board twice before these existed.
#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "storage/purge_logic.h"

using purge_logic::complete;
using purge_logic::is_our_file;
using purge_logic::Queue;
using purge_logic::scan_exhausted;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- the queue

static void queue_holds_pushes_in_order() {
  Queue q;
  TEST_ASSERT_TRUE(q.push("/a"));
  TEST_ASSERT_TRUE(q.push("/b"));
  TEST_ASSERT_EQUAL_size_t(2, q.size());
  TEST_ASSERT_EQUAL_STRING("/a", q.at(0));
  TEST_ASSERT_EQUAL_STRING("/b", q.at(1));
}

// THE BUG: directories are only removed in the final rmdir sweep, so every
// rescan of a parent sees its children again.
static void rescanning_a_parent_does_not_requeue_its_children() {
  Queue q;
  q.push("/");
  for (int rescan = 0; rescan < 50; rescan++) {
    q.push("/DCIM");
    q.push("/System Volume Information");
  }
  TEST_ASSERT_EQUAL_size_t(3, q.size());
  TEST_ASSERT_EQUAL_size_t(0, q.skipped());  // duplicates are not skipped work
}

static void duplicate_push_reports_false_but_is_not_an_overflow() {
  Queue q;
  TEST_ASSERT_TRUE(q.push("/x"));
  TEST_ASSERT_FALSE(q.push("/x"));
  TEST_ASSERT_EQUAL_size_t(0, q.skipped());
}

static void overflow_is_counted_so_the_caller_can_report_it() {
  Queue q;
  char p[32];
  for (size_t i = 0; i < purge_logic::kMaxDirs; i++) {
    snprintf(p, sizeof(p), "/d%zu", i);
    TEST_ASSERT_TRUE(q.push(p));
  }
  TEST_ASSERT_EQUAL_size_t(purge_logic::kMaxDirs, q.size());
  TEST_ASSERT_FALSE(q.push("/one-too-many"));
  TEST_ASSERT_EQUAL_size_t(1, q.skipped());
  // A duplicate at capacity is still not an overflow.
  TEST_ASSERT_FALSE(q.push("/d0"));
  TEST_ASSERT_EQUAL_size_t(1, q.skipped());
}

static void empty_and_null_paths_are_refused() {
  Queue q;
  TEST_ASSERT_FALSE(q.push(nullptr));
  TEST_ASSERT_FALSE(q.push(""));
  TEST_ASSERT_EQUAL_size_t(0, q.size());
}

static void long_paths_are_truncated_not_overflowed() {
  Queue q;
  char longp[300];
  memset(longp, 'x', sizeof(longp) - 1);
  longp[0] = '/';
  longp[sizeof(longp) - 1] = '\0';
  TEST_ASSERT_TRUE(q.push(longp));
  TEST_ASSERT_EQUAL_size_t(purge_logic::kMaxPathLen - 1, strlen(q.at(0)));
}

static void out_of_range_read_is_safe() {
  Queue q;
  q.push("/a");
  TEST_ASSERT_EQUAL_STRING("", q.at(5));
}

// THE OTHER BUG: 3 KB of batch buffer per RECURSION LEVEL smashed an 8 KB
// stack. The queue replaced that recursion, so what has to stay true is that
// its footprint is a compile-time constant -- it must not grow with the depth
// or breadth of the tree being walked. (Where it LIVES is now the caller's
// choice: sdcard.cpp puts it in a scratch block for the duration of a purge.)
static void queue_footprint_is_fixed_and_independent_of_tree_depth() {
  constexpr size_t kExpected = sizeof(char[purge_logic::kMaxDirs][purge_logic::kMaxPathLen]);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32((uint32_t)kExpected, (uint32_t)sizeof(Queue));
  // Bookkeeping on top of the paths, not a second copy of them.
  TEST_ASSERT_LESS_OR_EQUAL_UINT32((uint32_t)(kExpected + 64), (uint32_t)sizeof(Queue));
}

// ------------------------------------------------------------ loop control

static void a_pass_that_removed_nothing_stops_the_rescan() {
  // Every entry was a queued directory: rescanning returns the same names
  // forever, so the walk must not loop.
  TEST_ASSERT_TRUE(scan_exhausted(16, 0));
  TEST_ASSERT_TRUE(scan_exhausted(0, 0));
  TEST_ASSERT_FALSE(scan_exhausted(16, 16));
  TEST_ASSERT_FALSE(scan_exhausted(16, 1));  // progress, however slight
}

// ------------------------------------------------------- leftover accounting

static void our_own_files_are_not_foreign_leftovers() {
  TEST_ASSERT_TRUE(is_our_file("/events.log"));
  TEST_ASSERT_TRUE(is_our_file("/events.old"));
  TEST_ASSERT_TRUE(is_our_file("/climate-202608.csv"));
}

// FAT is case-insensitive and hands back 8.3 short names in upper case.
// Comparing full paths with strcmp is what made the fan's own log look like a
// foreign survivor.
static void matching_is_case_insensitive_and_basename_based() {
  TEST_ASSERT_TRUE(is_our_file("/EVENTS.LOG"));
  TEST_ASSERT_TRUE(is_our_file("/CLIMATE-202608.CSV"));
  TEST_ASSERT_TRUE(is_our_file("events.log"));  // no leading slash
  TEST_ASSERT_TRUE(is_our_file("/sd/events.log"));
}

static void foreign_entries_are_reported() {
  TEST_ASSERT_FALSE(is_our_file("/.Spotlight-V100"));
  TEST_ASSERT_FALSE(is_our_file("/DCIM"));
  TEST_ASSERT_FALSE(is_our_file("/holiday.mp4"));
  TEST_ASSERT_FALSE(is_our_file(nullptr));
  // Near-misses must not be waved through.
  TEST_ASSERT_FALSE(is_our_file("/events.log.bak"));
  TEST_ASSERT_FALSE(is_our_file("/myevents.log"));
  // A bare "climate-" prefix used to claim these, so a holiday photo counted
  // as one of ours and purge() could report the card clear with it still on it.
  TEST_ASSERT_FALSE(is_our_file("/Climate-2019-holiday.jpg"));
  TEST_ASSERT_FALSE(is_our_file("/climate-notes.txt"));
  // Only the exact shape log_sample() writes counts. A looser rule drops a
  // foreign file from the leftover count, so purge() reports the card clear
  // with it still on it.
  TEST_ASSERT_FALSE(is_our_file("/climate-notes.csv"));    // right suffix, wrong stem
  TEST_ASSERT_FALSE(is_our_file("/climate-2026.csv"));     // too few digits
  TEST_ASSERT_FALSE(is_our_file("/climate-2026081.csv"));  // too many
  TEST_ASSERT_FALSE(is_our_file("/climate-20260a.csv"));   // not all digits
  TEST_ASSERT_TRUE(is_our_file("/climate-202608.csv"));    // and the real one still matches
}

static void completeness_needs_all_three_conditions() {
  TEST_ASSERT_TRUE(complete(/*budget_left=*/100, /*skipped=*/0, /*remaining=*/0));
  TEST_ASSERT_FALSE(complete(0, 0, 0));    // ran out of allowance
  TEST_ASSERT_FALSE(complete(100, 1, 0));  // a directory never got visited
  TEST_ASSERT_FALSE(complete(100, 0, 1));  // something survived
}

// The regression that made the endpoint lie: 28 GB reclaimed, budget barely
// touched, and the only root entry was the log the purge itself rewrote.
static void a_finished_sweep_whose_only_leftover_is_ours_reads_complete() {
  const uint32_t remaining = is_our_file("/events.log") ? 0 : 1;
  TEST_ASSERT_TRUE(complete(/*budget_left=*/19792, /*skipped=*/0, remaining));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(queue_holds_pushes_in_order);
  RUN_TEST(rescanning_a_parent_does_not_requeue_its_children);
  RUN_TEST(duplicate_push_reports_false_but_is_not_an_overflow);
  RUN_TEST(overflow_is_counted_so_the_caller_can_report_it);
  RUN_TEST(empty_and_null_paths_are_refused);
  RUN_TEST(long_paths_are_truncated_not_overflowed);
  RUN_TEST(out_of_range_read_is_safe);
  RUN_TEST(queue_footprint_is_fixed_and_independent_of_tree_depth);
  RUN_TEST(a_pass_that_removed_nothing_stops_the_rescan);
  RUN_TEST(our_own_files_are_not_foreign_leftovers);
  RUN_TEST(matching_is_case_insensitive_and_basename_based);
  RUN_TEST(foreign_entries_are_reported);
  RUN_TEST(completeness_needs_all_three_conditions);
  RUN_TEST(a_finished_sweep_whose_only_leftover_is_ours_reads_complete);
  return UNITY_END();
}
