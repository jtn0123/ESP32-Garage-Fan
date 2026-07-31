// Unit tests for spec_render.h — the pure template expander and weather
// shortener behind every templated ui_spec op. Pure header, so these compile
// natively with no Arduino shim. The "in -- out --" regression (a template
// key the device could not resolve) is exactly the class these catch.
#include <unity.h>
#include <stdio.h>
#include <string.h>

#include "../../src/spec_render.h"

// Table-backed fake resolver standing in for spec_format_field_into().
static void fake_resolver(const char* key, char* out, size_t n) {
  if (strcmp(key, "name") == 0) {
    snprintf(out, n, "Office");
  } else if (strcmp(key, "temp") == 0) {
    snprintf(out, n, "72");
  } else if (strcmp(key, "long") == 0) {
    snprintf(out, n, "0123456789012345678901234567890123456789");
  } else {
    snprintf(out, n, "--");
  }
}

void test_literal_text_passes_through() {
  char out[32];
  spec_expand_template_into("plain text", fake_resolver, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("plain text", out);
}

void test_single_and_multiple_keys() {
  char out[48];
  spec_expand_template_into("{name}", fake_resolver, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Office", out);
  spec_expand_template_into("IN {temp} at {name}!", fake_resolver, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("IN 72 at Office!", out);
}

void test_unknown_key_renders_placeholder() {
  char out[32];
  spec_expand_template_into("{nope} hPa", fake_resolver, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("-- hPa", out);
}

void test_unterminated_brace_copies_literally() {
  char out[32];
  spec_expand_template_into("abc {temp", fake_resolver, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("abc {temp", out);
}

void test_truncation_is_safe_and_terminated() {
  char out[8];
  spec_expand_template_into("{long}{long}", fake_resolver, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT(7, strlen(out));
  TEST_ASSERT_EQUAL_STRING("0123456", out);
}

void test_null_and_empty_template() {
  char out[8];
  out[0] = 'x';
  spec_expand_template_into(NULL, fake_resolver, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("", out);
  spec_expand_template_into("", fake_resolver, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("", out);
}

void test_short_condition_keyword_order() {
  // "part" outranks "cloud": "Partly Cloudy" must not collapse to Cloudy.
  TEST_ASSERT_EQUAL_STRING("Partly", spec_short_condition("Partly Cloudy"));
  TEST_ASSERT_EQUAL_STRING("Night", spec_short_condition("clear-night"));
  TEST_ASSERT_EQUAL_STRING("Storm", spec_short_condition("Thunderstorm"));
  TEST_ASSERT_EQUAL_STRING("Cloudy", spec_short_condition("overcast"));
  TEST_ASSERT_EQUAL_STRING("Rain", spec_short_condition("light drizzle"));
}

void test_short_condition_case_and_fallbacks() {
  TEST_ASSERT_EQUAL_STRING("Rain", spec_short_condition("RAIN"));
  TEST_ASSERT_EQUAL_STRING("Sunny", spec_short_condition("something odd"));
  TEST_ASSERT_EQUAL_STRING("Sunny", spec_short_condition(NULL));
  TEST_ASSERT_EQUAL_STRING("Sunny", spec_short_condition(""));
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_literal_text_passes_through);
  RUN_TEST(test_single_and_multiple_keys);
  RUN_TEST(test_unknown_key_renders_placeholder);
  RUN_TEST(test_unterminated_brace_copies_literally);
  RUN_TEST(test_truncation_is_safe_and_terminated);
  RUN_TEST(test_null_and_empty_template);
  RUN_TEST(test_short_condition_keyword_order);
  RUN_TEST(test_short_condition_case_and_fallbacks);
  return UNITY_END();
}
