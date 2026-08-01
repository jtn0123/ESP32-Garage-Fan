// Unit tests for the link-health recovery policy (src/link_recovery.h).
// Compiled from the real header on platform = native — no hand-copied logic.

#include <unity.h>

#include "../../src/link_recovery.h"

void setUp(void) {}
void tearDown(void) {}

// Healthy link: nothing to do, counter stays clear.
void test_healthy_link_no_action(void) {
    LinkRecovery r;
    for (int i = 0; i < 50; i++) {
        TEST_ASSERT_EQUAL(static_cast<int>(LinkAction::kNone),
                          static_cast<int>(r.on_check(true, true, 0)));
    }
    TEST_ASSERT_EQUAL_UINT32(0, r.unreachable_checks);
}

// The bug this policy exists for: wifi "up", broker never reachable.
void test_escalates_radio_cycle_then_reboot(void) {
    LinkRecovery r;
    // Checks 1-3: below the radio-cycle threshold.
    for (int i = 1; i <= 3; i++) {
        TEST_ASSERT_EQUAL(static_cast<int>(LinkAction::kNone),
                          static_cast<int>(r.on_check(true, false, 0)));
    }
    // Check 4: radio cycle.
    TEST_ASSERT_EQUAL(static_cast<int>(LinkAction::kRadioCycle),
                      static_cast<int>(r.on_check(true, false, 0)));
    // Checks 5-9: waiting.
    for (int i = 5; i <= 9; i++) {
        TEST_ASSERT_EQUAL(static_cast<int>(LinkAction::kNone),
                          static_cast<int>(r.on_check(true, false, 0)));
    }
    // Check 10: reboot.
    TEST_ASSERT_EQUAL(static_cast<int>(LinkAction::kReboot),
                      static_cast<int>(r.on_check(true, false, 0)));
}

// A brief broker blip that resolves before the threshold must not escalate.
void test_broker_blip_resets_counter(void) {
    LinkRecovery r;
    r.on_check(true, false, 0);
    r.on_check(true, false, 0);
    // Broker back: counter clears.
    TEST_ASSERT_EQUAL(static_cast<int>(LinkAction::kNone),
                      static_cast<int>(r.on_check(true, true, 0)));
    TEST_ASSERT_EQUAL_UINT32(0, r.unreachable_checks);
    // Escalation starts over from scratch.
    for (int i = 1; i <= 3; i++) {
        TEST_ASSERT_EQUAL(static_cast<int>(LinkAction::kNone),
                          static_cast<int>(r.on_check(true, false, 0)));
    }
}

// WiFi fully down is the other branch's problem; the counter freezes rather
// than resetting, so a flapping AP cannot indefinitely defer escalation.
void test_wifi_down_freezes_counter(void) {
    LinkRecovery r;
    r.on_check(true, false, 0);
    r.on_check(true, false, 0);
    TEST_ASSERT_EQUAL_UINT32(2, r.unreachable_checks);
    TEST_ASSERT_EQUAL(static_cast<int>(LinkAction::kNone),
                      static_cast<int>(r.on_check(false, false, 0)));
    TEST_ASSERT_EQUAL_UINT32(2, r.unreachable_checks);  // frozen, not reset
    // WiFi returns, broker still absent: escalation resumes where it left off.
    r.on_check(true, false, 0);
    TEST_ASSERT_EQUAL(static_cast<int>(LinkAction::kRadioCycle),
                      static_cast<int>(r.on_check(true, false, 0)));
}

// Reboot budget spent (broker outage, not a wedged link): never reboot again,
// but keep nudging the radio periodically so a genuinely wedged link still
// recovers once the outage ends.
void test_reboot_budget_exhausted_falls_back_to_radio_cycles(void) {
    LinkRecovery r;
    int reboots = 0, cycles = 0;
    for (int i = 0; i < 40; i++) {
        LinkAction a = r.on_check(true, false, LINK_MAX_RECOVERY_REBOOTS);
        if (a == LinkAction::kReboot) reboots++;
        if (a == LinkAction::kRadioCycle) cycles++;
    }
    TEST_ASSERT_EQUAL(0, reboots);
    // Every LINK_RADIO_RESET_CHECKS-th check cycles: 40/4 = 10.
    TEST_ASSERT_EQUAL(10, cycles);
}

// Budget partially spent: reboots continue until the cap.
void test_reboot_budget_partially_spent_still_reboots(void) {
    LinkRecovery r;
    for (int i = 0; i < 9; i++) r.on_check(true, false, LINK_MAX_RECOVERY_REBOOTS - 1);
    TEST_ASSERT_EQUAL(static_cast<int>(LinkAction::kReboot),
                      static_cast<int>(r.on_check(true, false, LINK_MAX_RECOVERY_REBOOTS - 1)));
}

// After the reboot threshold, every subsequent check keeps asking to reboot
// (the caller reboots on the first one; this matters only if a reboot fails).
void test_keeps_requesting_reboot_past_threshold(void) {
    LinkRecovery r;
    for (int i = 0; i < 12; i++) r.on_check(true, false, 0);
    TEST_ASSERT_EQUAL(static_cast<int>(LinkAction::kReboot),
                      static_cast<int>(r.on_check(true, false, 0)));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_healthy_link_no_action);
    RUN_TEST(test_escalates_radio_cycle_then_reboot);
    RUN_TEST(test_broker_blip_resets_counter);
    RUN_TEST(test_wifi_down_freezes_counter);
    RUN_TEST(test_reboot_budget_exhausted_falls_back_to_radio_cycles);
    RUN_TEST(test_reboot_budget_partially_spent_still_reboots);
    RUN_TEST(test_keeps_requesting_reboot_past_threshold);
    return UNITY_END();
}
