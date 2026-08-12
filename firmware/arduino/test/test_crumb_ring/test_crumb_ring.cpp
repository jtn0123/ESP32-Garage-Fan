// Host tests for the RTC breadcrumb ring. See crumb_ring.h.
//
// This is the evidence the next crash depends on, and its first version
// shipped a bug that disabled it within a minute of uptime. These exist so
// that cannot happen quietly again.
#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "system/crumb_ring.h"

using crumb_ring::kSlots;
using crumb_ring::Ring;

static Ring r;

void setUp() {
  memset(&r, 0, sizeof(r));  // as RTC memory looks after a power-on
}
void tearDown() {}

static const char* rendered() {
  static char buf[256];
  r.render(buf, sizeof(buf));
  return buf;
}

// --------------------------------------------------------------- the basics

static void a_zeroed_ring_is_not_valid_and_renders_nothing() {
  // Power-on leaves RTC as noise; without the magic check the firmware would
  // print garbage as the previous run's trail.
  TEST_ASSERT_FALSE(r.valid());
  TEST_ASSERT_EQUAL_STRING("", rendered());
}

static void drops_render_oldest_first_with_timestamps() {
  r.reset();
  r.drop("setup", 10);
  r.drop("mqtt", 20);
  r.drop("epd", 30);
  TEST_ASSERT_EQUAL_STRING("setup@10 mqtt@20 epd@30", rendered());
}

static void the_first_drop_makes_the_ring_valid() {
  r.drop("boot", 1);  // no reset() first: magic was noise
  TEST_ASSERT_TRUE(r.valid());
  TEST_ASSERT_EQUAL_STRING("boot@1", rendered());
}

static void reset_clears_the_trail() {
  r.reset();
  r.drop("a", 1);
  r.reset();
  TEST_ASSERT_TRUE(r.valid());
  TEST_ASSERT_EQUAL_STRING("", rendered());
}

// ------------------------------------------------------------- the wrapping

static void a_full_ring_keeps_the_most_recent_slots() {
  r.reset();
  for (uint32_t i = 0; i < kSlots; i++) {
    char tag[8];
    snprintf(tag, sizeof(tag), "t%lu", (unsigned long)i);
    r.drop(tag, i);
  }
  r.drop("newest", 999);
  const char* out = rendered();
  TEST_ASSERT_NULL(strstr(out, "t0@0"));      // the oldest fell off
  TEST_ASSERT_NOT_NULL(strstr(out, "t1@1"));  // and the next is now first
  TEST_ASSERT_NOT_NULL(strstr(out, "newest@999"));
  // Oldest-first ordering survives the wrap.
  TEST_ASSERT_TRUE(strstr(out, "t1@1") < strstr(out, "newest@999"));
}

// THE SHIPPED BUG: validity was gated on next <= 0xFFFF. The loop drops
// several crumbs per iteration, so that was passed in under a minute and the
// ring went silent for the rest of the run -- disabling itself on exactly the
// boot after a long-running fault.
static void the_ring_survives_far_more_than_65536_drops() {
  r.reset();
  for (uint32_t i = 0; i < 70000; i++) r.drop("x", i);
  TEST_ASSERT_TRUE(r.valid());
  TEST_ASSERT_LESS_THAN_UINT8(kSlots, r.next);
  const char* out = rendered();
  TEST_ASSERT_NOT_NULL(strstr(out, "x@69999"));  // the newest is still there
}

// The cursor must not be able to overflow at all. It used to be a running
// total: the loop drops several crumbs per iteration, so a uint32_t wrapped
// after about three weeks of uptime and the trail rendered EMPTY at the wrap,
// on a device that is meant to run for months.
static void a_very_long_uptime_never_blanks_the_trail() {
  r.reset();
  for (uint32_t i = 0; i < 200000; i++) {
    r.drop("x", i);
    if ((i % 9973) == 0) {  // sample often, at coprime offsets
      TEST_ASSERT_TRUE(r.valid());
      TEST_ASSERT_TRUE(strlen(rendered()) > 0);
    }
  }
  TEST_ASSERT_TRUE(r.valid());
  TEST_ASSERT_NOT_NULL(strstr(rendered(), "x@199999"));
  TEST_ASSERT_LESS_OR_EQUAL_UINT8(kSlots, r.count);
  TEST_ASSERT_LESS_THAN_UINT8(kSlots, r.next);
}

// ----------------------------------------------------------------- hardening

static void an_over_long_tag_is_truncated_not_overflowed() {
  r.reset();
  r.drop("a-very-long-breadcrumb-tag-indeed", 5);
  TEST_ASSERT_EQUAL_size_t(crumb_ring::kTagLen - 1, strlen(r.entries[0].tag));
  TEST_ASSERT_NOT_NULL(strstr(rendered(), "@5"));
}

static void empty_and_null_tags_are_ignored() {
  r.reset();
  r.drop(nullptr, 1);
  r.drop("", 2);
  TEST_ASSERT_EQUAL_UINT8(0, r.count);
  TEST_ASSERT_EQUAL_STRING("", rendered());
}

// The trail goes into a fixed eventlog line; a clipped tail is fine, a
// buffer overrun is not.
static void render_into_a_small_buffer_truncates_safely() {
  r.reset();
  for (int i = 0; i < kSlots; i++) r.drop("stage", 123456);
  char small[24];
  r.render(small, sizeof(small));
  TEST_ASSERT_TRUE(strlen(small) < sizeof(small));
  TEST_ASSERT_NOT_NULL(strstr(small, "stage@123456"));  // earliest steps kept
}

static void render_handles_a_degenerate_buffer() {
  r.reset();
  r.drop("a", 1);
  char one[1];
  r.render(one, sizeof(one));
  TEST_ASSERT_EQUAL_STRING("", one);
  r.render(nullptr, 0);  // must not crash
}

// The real shape: a boot, some loop stages, then the fall. This is what the
// tape should show after the unexplained post-purge panic recurs.
static void a_realistic_trail_reads_as_a_sequence() {
  r.reset();
  r.drop("setup", 0);
  r.drop("mqtt", 3000);
  r.drop("sd_purge", 24000);
  r.drop("epd", 76000);
  r.drop("web", 91000);
  TEST_ASSERT_EQUAL_STRING("setup@0 mqtt@3000 sd_purge@24000 epd@76000 web@91000", rendered());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(a_zeroed_ring_is_not_valid_and_renders_nothing);
  RUN_TEST(drops_render_oldest_first_with_timestamps);
  RUN_TEST(the_first_drop_makes_the_ring_valid);
  RUN_TEST(reset_clears_the_trail);
  RUN_TEST(a_full_ring_keeps_the_most_recent_slots);
  RUN_TEST(the_ring_survives_far_more_than_65536_drops);
  RUN_TEST(a_very_long_uptime_never_blanks_the_trail);
  RUN_TEST(an_over_long_tag_is_truncated_not_overflowed);
  RUN_TEST(empty_and_null_tags_are_ignored);
  RUN_TEST(render_into_a_small_buffer_truncates_safely);
  RUN_TEST(render_handles_a_degenerate_buffer);
  RUN_TEST(a_realistic_trail_reads_as_a_sequence);
  return UNITY_END();
}
