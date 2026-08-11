// Host tests for the flight-recorder noise gate. See log_filter.h.
#include <unity.h>

#include "system/log_filter.h"

using log_filter::is_i2c_noise;
using log_filter::NoiseGate;

void setUp() {}
void tearDown() {}

static void recognises_the_two_real_lines() {
  // Verbatim from the deployed board's /api/events tail.
  TEST_ASSERT_TRUE(is_i2c_noise(
      "E (1337409) ARDUINO: i2c_master_transmit_receive failed: [259] ESP_ERR_INVALID_"));
  TEST_ASSERT_TRUE(is_i2c_noise("E (1337409) ARDUINO: i2cWriteReadNonStop returned Error 259 "));
}

static void leaves_everything_else_alone() {
  TEST_ASSERT_FALSE(is_i2c_noise("E (123) ARDUINO: Brownout detector was triggered"));
  TEST_ASSERT_FALSE(is_i2c_noise("W (99) wifi: disconnected"));
  TEST_ASSERT_FALSE(is_i2c_noise(""));
  TEST_ASSERT_FALSE(is_i2c_noise(nullptr));
}

// A real fault must never be suppressed, however much noise preceded it.
static void non_noise_always_emits() {
  NoiseGate g;
  for (int i = 0; i < 500; i++) g.feed(true, 1000);
  const auto v = g.feed(false, 1000);
  TEST_ASSERT_TRUE(v.emit);
  TEST_ASSERT_FALSE(v.summary);
}

static void keeps_the_first_few_then_suppresses() {
  NoiseGate g;
  for (int i = 0; i < NoiseGate::kKeep; i++) {
    const auto v = g.feed(true, 1000);
    TEST_ASSERT_TRUE(v.emit);
  }
  const auto v = g.feed(true, 1000);
  TEST_ASSERT_FALSE(v.emit);
  TEST_ASSERT_FALSE(v.summary);
}

static void summarises_after_the_interval_and_resets_the_count() {
  NoiseGate g;
  for (int i = 0; i < NoiseGate::kKeep; i++) g.feed(true, 0);
  for (int i = 0; i < 10; i++) g.feed(true, 0);  // suppressed, counted
  const auto v = g.feed(true, NoiseGate::kSummaryMs + 1);
  TEST_ASSERT_TRUE(v.summary);
  TEST_ASSERT_FALSE(v.emit);
  TEST_ASSERT_EQUAL_UINT32(11, v.count);  // the 10 plus the one that triggered it
  TEST_ASSERT_EQUAL_UINT32(0, g.suppressed());
}

// The whole point: at four lines per 30 s, a day of chatter must not be able
// to evict the tape, and must still be reported.
static void a_day_of_chatter_emits_only_a_handful_of_lines() {
  NoiseGate g;
  uint32_t emitted = 0, summaries = 0;
  for (uint32_t ms = 0; ms < 24UL * 3600UL * 1000UL; ms += 30000) {
    for (int i = 0; i < 4; i++) {
      const auto v = g.feed(true, ms);
      if (v.emit)
        emitted++;
      if (v.summary)
        summaries++;
    }
  }
  TEST_ASSERT_EQUAL_UINT32(NoiseGate::kKeep, emitted);
  TEST_ASSERT_EQUAL_UINT32(23, summaries);  // hourly, first hour still filling
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(recognises_the_two_real_lines);
  RUN_TEST(leaves_everything_else_alone);
  RUN_TEST(non_noise_always_emits);
  RUN_TEST(keeps_the_first_few_then_suppresses);
  RUN_TEST(summarises_after_the_interval_and_resets_the_count);
  RUN_TEST(a_day_of_chatter_emits_only_a_handful_of_lines);
  return UNITY_END();
}
