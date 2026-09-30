#include <unity.h>

#include "net/health_status.h"

void setUp(void) {}
void tearDown(void) {}

void test_every_readiness_combination(void) {
  // Exercise the production decision across all inputs, including manual mode.
  for (unsigned mask = 0; mask < 128; ++mask) {
    const health_status::Snapshot s = {!!(mask & 1),  !!(mask & 2),  !!(mask & 4), !!(mask & 8),
                                       !!(mask & 16), !!(mask & 32), !!(mask & 64)};
    const bool expected = (mask & 99) == 99 && (!(mask & 4) || (mask & 24) == 24);
    TEST_ASSERT_EQUAL(expected, health_status::ready(s));
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_readiness_combination);
  return UNITY_END();
}
