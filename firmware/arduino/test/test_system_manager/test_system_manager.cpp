// Unit tests for the pure half of the system_manager module.
//
// This includes the REAL src/system_pure.h, so reset-reason classification and
// CRC32 are tested as implemented, not as re-typed into the test.
//
// What is deliberately NOT covered here, and why:
//   get_memory_diagnostics()  needs esp_heap_caps; there is no host equivalent
//                             whose numbers would mean anything.
//   nvs_*()                   needs Preferences/NVS flash.
//   wake count, deep sleep    needs RTC slow memory that survives a sleep cycle.
// The previous version of this file "covered" those with a series of
// TEST_ASSERT_TRUE(true) calls after each one, which asserted nothing while
// reading like passing coverage. They are omitted rather than faked -- those
// paths need on-target verification.
//
// It also called get_reset_reason_string()/fast_crc32() from system_manager.h,
// which transitively includes Preferences.h and so could never compile on
// platform = native. Both are now reachable via system_pure.h.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../../src/system_pure.h"

void setUp() {}
void tearDown() {}

// === reset reason strings ====================================================

void test_reset_reason_strings_are_mapped() {
  TEST_ASSERT_EQUAL_STRING("POWERON", reset_reason_to_string(ESP_RST_POWERON));
  TEST_ASSERT_EQUAL_STRING("EXTERNAL", reset_reason_to_string(ESP_RST_EXT));
  TEST_ASSERT_EQUAL_STRING("SOFTWARE", reset_reason_to_string(ESP_RST_SW));
  TEST_ASSERT_EQUAL_STRING("PANIC", reset_reason_to_string(ESP_RST_PANIC));
  TEST_ASSERT_EQUAL_STRING("INT_WATCHDOG", reset_reason_to_string(ESP_RST_INT_WDT));
  TEST_ASSERT_EQUAL_STRING("TASK_WATCHDOG", reset_reason_to_string(ESP_RST_TASK_WDT));
  TEST_ASSERT_EQUAL_STRING("WATCHDOG", reset_reason_to_string(ESP_RST_WDT));
  TEST_ASSERT_EQUAL_STRING("DEEPSLEEP", reset_reason_to_string(ESP_RST_DEEPSLEEP));
  TEST_ASSERT_EQUAL_STRING("BROWNOUT", reset_reason_to_string(ESP_RST_BROWNOUT));
  TEST_ASSERT_EQUAL_STRING("SDIO", reset_reason_to_string(ESP_RST_SDIO));
  TEST_ASSERT_EQUAL_STRING("UNKNOWN", reset_reason_to_string(ESP_RST_UNKNOWN));
}

void test_reset_reason_never_returns_null() {
  // These strings go straight into printf and MQTT payloads, so a null for an
  // unmapped enumerator would be a crash rather than a cosmetic problem.
  for (int i = -5; i < 40; i++) {
    const char* s = reset_reason_to_string(static_cast<esp_reset_reason_t>(i));
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(strlen(s) > 0);
  }
}

void test_unmapped_reason_falls_back_to_unknown() {
  TEST_ASSERT_EQUAL_STRING("UNKNOWN", reset_reason_to_string(static_cast<esp_reset_reason_t>(99)));
}

// === crash classification ====================================================

void test_crash_reasons_are_flagged() {
  TEST_ASSERT_TRUE(reset_reason_indicates_crash(ESP_RST_PANIC));
  TEST_ASSERT_TRUE(reset_reason_indicates_crash(ESP_RST_INT_WDT));
  TEST_ASSERT_TRUE(reset_reason_indicates_crash(ESP_RST_TASK_WDT));
  TEST_ASSERT_TRUE(reset_reason_indicates_crash(ESP_RST_WDT));
  TEST_ASSERT_TRUE(reset_reason_indicates_crash(ESP_RST_BROWNOUT));
}

void test_normal_reasons_are_not_flagged() {
  // DEEPSLEEP is the single most common reset on this device -- treating it as a
  // crash would report a fault on every scheduled wake.
  TEST_ASSERT_FALSE(reset_reason_indicates_crash(ESP_RST_DEEPSLEEP));
  TEST_ASSERT_FALSE(reset_reason_indicates_crash(ESP_RST_POWERON));
  TEST_ASSERT_FALSE(reset_reason_indicates_crash(ESP_RST_SW));
  TEST_ASSERT_FALSE(reset_reason_indicates_crash(ESP_RST_EXT));
  TEST_ASSERT_FALSE(reset_reason_indicates_crash(ESP_RST_UNKNOWN));
  TEST_ASSERT_FALSE(reset_reason_indicates_crash(ESP_RST_SDIO));
}

// === CRC32 ===================================================================

void test_crc32_is_deterministic() {
  const uint8_t data[] = "Hello";
  uint32_t a = crc32_bytes(data, 5);
  uint32_t b = crc32_bytes(data, 5);
  TEST_ASSERT_EQUAL_UINT32(a, b);
}

void test_crc32_matches_known_value() {
  // Standard CRC-32 (poly 0xEDB88320, init 0xFFFFFFFF, final XOR) of "123456789"
  // is 0xCBF43926. Pinning a published value catches a change to any of the
  // three parameters, which a self-consistency check alone would not.
  const uint8_t data[] = "123456789";
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32_bytes(data, 9));
}

void test_crc32_empty_input_is_zero() {
  // ~0xFFFFFFFF == 0 for a zero-length run.
  const uint8_t data[] = "unused";
  TEST_ASSERT_EQUAL_UINT32(0u, crc32_bytes(data, 0));
}

void test_crc32_distinguishes_similar_inputs() {
  const uint8_t a[] = "Hello";
  const uint8_t b[] = "World";
  TEST_ASSERT_NOT_EQUAL(crc32_bytes(a, 5), crc32_bytes(b, 5));

  // The status line differs by a single character between refreshes far more
  // often than not, so single-bit sensitivity is the property that matters.
  const uint8_t c[] = "72.4F";
  const uint8_t d[] = "72.5F";
  TEST_ASSERT_NOT_EQUAL(crc32_bytes(c, 5), crc32_bytes(d, 5));
}

void test_crc32_is_length_sensitive() {
  // "ab" and "ab\0" must not collide just because the prefix matches.
  const uint8_t data[] = {'a', 'b', 0};
  TEST_ASSERT_NOT_EQUAL(crc32_bytes(data, 2), crc32_bytes(data, 3));
}

void test_crc32_handles_binary_and_high_bytes() {
  const uint8_t binary[] = {0x00, 0xFF, 0xAA, 0x55};
  uint32_t crc = crc32_bytes(binary, 4);
  TEST_ASSERT_NOT_EQUAL(0u, crc);

  // Leading NULs must still contribute; a strlen-based implementation would
  // treat this as empty.
  const uint8_t leading_nul[] = {0x00, 0x01};
  TEST_ASSERT_NOT_EQUAL(crc32_bytes(leading_nul, 2), crc32_bytes(leading_nul, 0));
}

void test_crc32_null_pointer_is_safe() {
  TEST_ASSERT_EQUAL_UINT32(0u, crc32_bytes(nullptr, 16));
}

// Native entry point. These tests run on platform = native, which has no
// Arduino setup()/loop() runtime to call into.
int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  RUN_TEST(test_reset_reason_strings_are_mapped);
  RUN_TEST(test_reset_reason_never_returns_null);
  RUN_TEST(test_unmapped_reason_falls_back_to_unknown);

  RUN_TEST(test_crash_reasons_are_flagged);
  RUN_TEST(test_normal_reasons_are_not_flagged);

  RUN_TEST(test_crc32_is_deterministic);
  RUN_TEST(test_crc32_matches_known_value);
  RUN_TEST(test_crc32_empty_input_is_zero);
  RUN_TEST(test_crc32_distinguishes_similar_inputs);
  RUN_TEST(test_crc32_is_length_sensitive);
  RUN_TEST(test_crc32_handles_binary_and_high_bytes);
  RUN_TEST(test_crc32_null_pointer_is_safe);

  return UNITY_END();
}
