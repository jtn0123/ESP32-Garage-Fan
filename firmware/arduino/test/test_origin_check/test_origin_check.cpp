// Host tests for the CSRF Origin guard. See origin_check.h.
//
// This is the check standing in front of every route that can change the fan.
#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "net/origin_check.h"

using net::origin_is_self;

static const char* IP = "192.0.2.187";
static const char* HOST = "garage-fan";

static bool self(const char* origin) { return origin_is_self(origin, IP, HOST); }

void setUp() {}
void tearDown() {}

// ------------------------------------------------------------------ accepted

static void the_console_served_from_this_device_is_accepted() {
  TEST_ASSERT_TRUE(self("http://192.0.2.187"));
  TEST_ASSERT_TRUE(self("http://garage-fan"));
  TEST_ASSERT_TRUE(self("http://garage-fan.local"));
}

// Browsers lowercase the host, but nothing forces them to; the guard must not
// depend on that.
static void matching_is_case_insensitive() {
  TEST_ASSERT_TRUE(self("HTTP://GARAGE-FAN.LOCAL"));
  TEST_ASSERT_TRUE(self("http://Garage-Fan"));
}

// curl, deploy.sh, and every other non-browser client send no Origin at all.
// Rejecting them would break the bench tooling for no security gain: CORS is
// a browser-enforced policy and a script can send whatever headers it likes.
static void an_absent_origin_is_allowed() {
  TEST_ASSERT_TRUE(self(nullptr));
  TEST_ASSERT_TRUE(self(""));
}

// ------------------------------------------------------------------ rejected

static void a_foreign_site_is_rejected() {
  TEST_ASSERT_FALSE(self("http://evil.example"));
  TEST_ASSERT_FALSE(self("https://evil.example"));
  TEST_ASSERT_FALSE(self("http://192.168.1.5"));
}

// THE ATTACK a prefix comparison would let through. These all start with a
// legitimate origin and are entirely different sites.
static void lookalike_hosts_are_rejected() {
  TEST_ASSERT_FALSE(self("http://garage-fan.local.evil.example"));
  TEST_ASSERT_FALSE(self("http://garage-fan.evil.example"));
  TEST_ASSERT_FALSE(self("http://192.0.2.187.evil.example"));
  TEST_ASSERT_FALSE(self("http://garage-fanX"));
  TEST_ASSERT_FALSE(self("http://192.0.2.1870"));
}

// A sandboxed iframe or a file:// page sends the literal string "null". It is
// not this device, and must not be mistaken for "no Origin".
static void the_literal_null_origin_is_rejected() {
  TEST_ASSERT_FALSE(self("null"));
  TEST_ASSERT_FALSE(self("NULL"));
}

// A different scheme or an explicit port is a different origin per the spec,
// and this device serves the console on plain HTTP port 80.
static void other_schemes_and_ports_are_rejected() {
  TEST_ASSERT_FALSE(self("https://garage-fan.local"));
  TEST_ASSERT_FALSE(self("http://garage-fan.local:8080"));
  TEST_ASSERT_FALSE(self("ftp://garage-fan"));
}

static void a_substring_of_a_valid_origin_is_rejected() {
  TEST_ASSERT_FALSE(self("http://garage-fan.loc"));
  TEST_ASSERT_FALSE(self("http://192.0.2.18"));
  TEST_ASSERT_FALSE(self("garage-fan.local"));  // no scheme
}

// --------------------------------------------------------------- robustness

static void missing_device_identity_does_not_open_the_door() {
  // Before WiFi is up the IP can be empty. That must not turn "http://" into
  // a match for anything.
  TEST_ASSERT_FALSE(origin_is_self("http://", "", HOST));
  TEST_ASSERT_FALSE(origin_is_self("http://evil.example", "", ""));
  TEST_ASSERT_FALSE(origin_is_self("http://evil.example", nullptr, nullptr));
}

static void an_overlong_origin_is_rejected_without_overflowing() {
  char big[600];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  TEST_ASSERT_FALSE(self(big));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(the_console_served_from_this_device_is_accepted);
  RUN_TEST(matching_is_case_insensitive);
  RUN_TEST(an_absent_origin_is_allowed);
  RUN_TEST(a_foreign_site_is_rejected);
  RUN_TEST(lookalike_hosts_are_rejected);
  RUN_TEST(the_literal_null_origin_is_rejected);
  RUN_TEST(other_schemes_and_ports_are_rejected);
  RUN_TEST(a_substring_of_a_valid_origin_is_rejected);
  RUN_TEST(missing_device_identity_does_not_open_the_door);
  RUN_TEST(an_overlong_origin_is_rejected_without_overflowing);
  return UNITY_END();
}
