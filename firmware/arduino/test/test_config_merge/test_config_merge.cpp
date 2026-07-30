// Unit tests for config_merge.h
//
// These helpers decide whether a value from /config/device.json on the SD card
// is allowed to override the firmware's compiled-in setting. The contract is
// what keeps a blank, partial, or malformed card from breaking a working
// device, so it is worth pinning down precisely.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../../src/config_merge.h"

// === merge_str ===============================================================

void test_merge_str_applies_non_empty() {
  char dst[16] = "compiled";
  TEST_ASSERT_TRUE(merge_str(dst, sizeof(dst), "from-card"));
  TEST_ASSERT_EQUAL_STRING("from-card", dst);
}

void test_merge_str_ignores_empty_string() {
  // The blank fields in the shipped device.json template must be no-ops.
  char dst[16] = "compiled";
  TEST_ASSERT_FALSE(merge_str(dst, sizeof(dst), ""));
  TEST_ASSERT_EQUAL_STRING("compiled", dst);
}

void test_merge_str_ignores_null_source() {
  char dst[16] = "compiled";
  TEST_ASSERT_FALSE(merge_str(dst, sizeof(dst), nullptr));
  TEST_ASSERT_EQUAL_STRING("compiled", dst);
}

void test_merge_str_truncates_and_terminates() {
  // An over-long SSID must not run off the end of the field.
  char dst[8];
  TEST_ASSERT_TRUE(merge_str(dst, sizeof(dst), "0123456789abcdef"));
  TEST_ASSERT_EQUAL_STRING("0123456", dst);
  TEST_ASSERT_EQUAL_CHAR('\0', dst[7]);
}

void test_merge_str_exact_fit_keeps_all_characters() {
  char dst[8];
  TEST_ASSERT_TRUE(merge_str(dst, sizeof(dst), "1234567"));
  TEST_ASSERT_EQUAL_STRING("1234567", dst);
}

void test_merge_str_rejects_null_dest_and_zero_size() {
  TEST_ASSERT_FALSE(merge_str(nullptr, 16, "x"));
  char dst[4] = "ab";
  TEST_ASSERT_FALSE(merge_str(dst, 0, "x"));
  TEST_ASSERT_EQUAL_STRING("ab", dst);
}

// === merge_u32 ==============================================================

void test_merge_u32_applies_in_range() {
  uint32_t dst = 300;
  TEST_ASSERT_TRUE(merge_u32(&dst, 600, 60, 3600));
  TEST_ASSERT_EQUAL_UINT32(600, dst);
}

void test_merge_u32_rejects_absent_sentinel() {
  // rc_apply_json() passes -1 to mean "key not present in the document".
  uint32_t dst = 300;
  TEST_ASSERT_FALSE(merge_u32(&dst, -1, 60, 3600));
  TEST_ASSERT_EQUAL_UINT32(300, dst);
}

void test_merge_u32_rejects_out_of_range_rather_than_clamping() {
  uint32_t dst = 300;
  TEST_ASSERT_FALSE(merge_u32(&dst, 10, 60, 3600));
  TEST_ASSERT_EQUAL_UINT32(300, dst);

  TEST_ASSERT_FALSE(merge_u32(&dst, 999999, 60, 3600));
  TEST_ASSERT_EQUAL_UINT32(300, dst);
}

void test_merge_u32_bounds_are_inclusive() {
  uint32_t dst = 300;
  TEST_ASSERT_TRUE(merge_u32(&dst, 60, 60, 3600));
  TEST_ASSERT_EQUAL_UINT32(60, dst);

  TEST_ASSERT_TRUE(merge_u32(&dst, 3600, 60, 3600));
  TEST_ASSERT_EQUAL_UINT32(3600, dst);
}

void test_merge_u32_allows_zero_when_range_permits() {
  // storage.history_retention_days uses 0 to mean "keep everything".
  uint32_t dst = 90;
  TEST_ASSERT_TRUE(merge_u32(&dst, 0, 0, 3650));
  TEST_ASSERT_EQUAL_UINT32(0, dst);
}

void test_merge_u32_rejects_null_dest() {
  TEST_ASSERT_FALSE(merge_u32(nullptr, 100, 0, 200));
}

// === merge_u16 ==============================================================

void test_merge_u16_applies_valid_port() {
  uint16_t dst = 1883;
  TEST_ASSERT_TRUE(merge_u16(&dst, 8883, 1, 65535));
  TEST_ASSERT_EQUAL_UINT16(8883, dst);
}

void test_merge_u16_rejects_port_zero_and_overflow() {
  uint16_t dst = 1883;
  TEST_ASSERT_FALSE(merge_u16(&dst, 0, 1, 65535));
  TEST_ASSERT_EQUAL_UINT16(1883, dst);

  // 65536 would wrap to 0 if it were cast without the range check.
  TEST_ASSERT_FALSE(merge_u16(&dst, 65536, 1, 65535));
  TEST_ASSERT_EQUAL_UINT16(1883, dst);
}

void test_merge_u16_rejects_null_dest() {
  TEST_ASSERT_FALSE(merge_u16(nullptr, 100, 1, 65535));
}

// === merge_bool =============================================================

void test_merge_bool_applies_when_present() {
  bool dst = true;
  TEST_ASSERT_TRUE(merge_bool(&dst, true, false));
  TEST_ASSERT_FALSE(dst);
}

void test_merge_bool_ignores_when_absent() {
  // A JSON document with no "enabled" key must leave the default alone, even
  // though the value argument defaults to false.
  bool dst = true;
  TEST_ASSERT_FALSE(merge_bool(&dst, false, false));
  TEST_ASSERT_TRUE(dst);
}

void test_merge_bool_rejects_null_dest() {
  TEST_ASSERT_FALSE(merge_bool(nullptr, true, true));
}

// === realistic scenarios ====================================================

void test_blank_template_leaves_everything_compiled_in() {
  // Exactly the shipped /config/device.json: every string blank, so nothing at
  // all should change.
  char ssid[33] = "HomeNetwork";
  char pass[65] = "secret";
  uint16_t port = 1883;

  TEST_ASSERT_FALSE(merge_str(ssid, sizeof(ssid), ""));
  TEST_ASSERT_FALSE(merge_str(pass, sizeof(pass), ""));
  TEST_ASSERT_FALSE(merge_u16(&port, -1, 1, 65535));

  TEST_ASSERT_EQUAL_STRING("HomeNetwork", ssid);
  TEST_ASSERT_EQUAL_STRING("secret", pass);
  TEST_ASSERT_EQUAL_UINT16(1883, port);
}

void test_partial_card_overrides_only_named_fields() {
  // A card that sets the host but not the credentials.
  char host[65] = "192.168.1.10";
  char user[33] = "builtin_user";
  uint16_t port = 1883;

  TEST_ASSERT_TRUE(merge_str(host, sizeof(host), "mqtt.local"));
  TEST_ASSERT_FALSE(merge_str(user, sizeof(user), ""));
  TEST_ASSERT_TRUE(merge_u16(&port, 8883, 1, 65535));

  TEST_ASSERT_EQUAL_STRING("mqtt.local", host);
  TEST_ASSERT_EQUAL_STRING("builtin_user", user);
  TEST_ASSERT_EQUAL_UINT16(8883, port);
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  UNITY_BEGIN();

  RUN_TEST(test_merge_str_applies_non_empty);
  RUN_TEST(test_merge_str_ignores_empty_string);
  RUN_TEST(test_merge_str_ignores_null_source);
  RUN_TEST(test_merge_str_truncates_and_terminates);
  RUN_TEST(test_merge_str_exact_fit_keeps_all_characters);
  RUN_TEST(test_merge_str_rejects_null_dest_and_zero_size);

  RUN_TEST(test_merge_u32_applies_in_range);
  RUN_TEST(test_merge_u32_rejects_absent_sentinel);
  RUN_TEST(test_merge_u32_rejects_out_of_range_rather_than_clamping);
  RUN_TEST(test_merge_u32_bounds_are_inclusive);
  RUN_TEST(test_merge_u32_allows_zero_when_range_permits);
  RUN_TEST(test_merge_u32_rejects_null_dest);

  RUN_TEST(test_merge_u16_applies_valid_port);
  RUN_TEST(test_merge_u16_rejects_port_zero_and_overflow);
  RUN_TEST(test_merge_u16_rejects_null_dest);

  RUN_TEST(test_merge_bool_applies_when_present);
  RUN_TEST(test_merge_bool_ignores_when_absent);
  RUN_TEST(test_merge_bool_rejects_null_dest);

  RUN_TEST(test_blank_template_leaves_everything_compiled_in);
  RUN_TEST(test_partial_card_overrides_only_named_fields);

  return UNITY_END();
}
