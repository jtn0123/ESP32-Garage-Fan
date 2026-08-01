// Unit tests for the boot-health rollback decision (src/boot_health.h).
// Compiled from the real header on platform = native — no hand-copied logic.

#include <unity.h>

#include "../../src/boot_health.h"

void setUp(void) {}
void tearDown(void) {}

// A confirmed image never rolls back, no matter how long the broker is gone:
// flipping slots would regress the firmware without fixing the outage.
void test_confirmed_image_never_rolls_back(void) {
    TEST_ASSERT_FALSE(boot_health_should_rollback(1, true));
    TEST_ASSERT_FALSE(boot_health_should_rollback(BOOT_HEALTH_MAX_UNHEALTHY, true));
    TEST_ASSERT_FALSE(boot_health_should_rollback(1000, true));
}

// An unconfirmed image gets BOOT_HEALTH_MAX_UNHEALTHY chances, then flips.
void test_unconfirmed_image_rolls_back_at_threshold(void) {
    for (uint32_t n = 1; n < BOOT_HEALTH_MAX_UNHEALTHY; n++) {
        TEST_ASSERT_FALSE(boot_health_should_rollback(n, false));
    }
    TEST_ASSERT_TRUE(boot_health_should_rollback(BOOT_HEALTH_MAX_UNHEALTHY, false));
    TEST_ASSERT_TRUE(boot_health_should_rollback(BOOT_HEALTH_MAX_UNHEALTHY + 1, false));
}

// The count includes the boot being decided, so the threshold is reachable on
// the link-recovery reboot budget's LAST boot (3 reboots -> 3rd boot decides)
// rather than needing a fourth boot the budget would never grant.
void test_threshold_reachable_within_reboot_budget(void) {
    // The link-recovery budget is 3 reboots; boots seen = budget count.
    TEST_ASSERT_TRUE(BOOT_HEALTH_MAX_UNHEALTHY <= 3);
}

// Zero unhealthy boots (fresh power-on that reset RTC) never rolls back.
void test_fresh_count_no_rollback(void) {
    TEST_ASSERT_FALSE(boot_health_should_rollback(0, false));
    TEST_ASSERT_FALSE(boot_health_should_rollback(0, true));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_confirmed_image_never_rolls_back);
    RUN_TEST(test_unconfirmed_image_rolls_back_at_threshold);
    RUN_TEST(test_threshold_reachable_within_reboot_budget);
    RUN_TEST(test_fresh_count_no_rollback);
    return UNITY_END();
}
