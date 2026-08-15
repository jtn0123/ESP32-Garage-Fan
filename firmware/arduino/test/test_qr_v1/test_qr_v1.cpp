// The C QR encoder, pinned to the same segno reference as the Python twin.
//
// tests/test_qr_v1.py pins scripts/mock_device.py's port; this pins
// ui/qr_v1.h. Both carry the identical expected matrices (segno, error='l',
// mask=0, micro and error-boost off), so a change to either port that
// diverges from the reference fails in CI instead of on the glass.
#include <unity.h>

#include <cstdio>
#include <cstring>

#include "ui/qr_v1.h"

// Each row of the 21x21 matrix packed as 21 bits, MSB = x0 -- the same
// encoding tests/test_qr_v1.py stores.
static const uint32_t kLoopback[21] = {
    0x1fc57f, 0x104141, 0x17545d, 0x17415d, 0x174b5d, 0x104e41, 0x1fd57f,
    0x001400, 0x1df5c4, 0x18b67f, 0x127a60, 0x188b00, 0x1256d1, 0x001a71,
    0x1fd9fd, 0x1056e6, 0x175558, 0x174aee, 0x17588d, 0x1054e7, 0x1fd5a9};
static const uint32_t kGarage[21] = {
    0x1fc57f, 0x104141, 0x17545d, 0x17415d, 0x17495d, 0x104e41, 0x1fd57f,
    0x001600, 0x1df5c4, 0x062a6f, 0x0bca50, 0x0dacb0, 0x1a7d81, 0x001071,
    0x1fdfdc, 0x105de4, 0x1751d8, 0x1742ae, 0x17504d, 0x1057e7, 0x1fdda9};

static void assert_matrix(const char* payload, const uint32_t* want) {
  uint8_t out[qr_v1::kSize][qr_v1::kSize];
  TEST_ASSERT_TRUE_MESSAGE(qr_v1::encode(payload, out), payload);
  for (int y = 0; y < qr_v1::kSize; y++) {
    uint32_t row = 0;
    for (int x = 0; x < qr_v1::kSize; x++) row = (row << 1) | (out[y][x] ? 1u : 0u);
    char msg[48];
    snprintf(msg, sizeof(msg), "%s row %d", payload, y);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want[y], row, msg);
  }
}

static void the_loopback_url_matches_the_reference() {
  assert_matrix("HTTP://127.0.0.1", kLoopback);
}

static void the_deployed_url_matches_the_reference() {
  assert_matrix("HTTP://10.27.27.187", kGarage);
}

static void lowercase_is_refused_not_guessed() {
  uint8_t out[qr_v1::kSize][qr_v1::kSize];
  TEST_ASSERT_FALSE(qr_v1::encode("http://127.0.0.1", out));
}

static void the_capacity_boundary_is_exact() {
  uint8_t out[qr_v1::kSize][qr_v1::kSize];
  char s26[27];
  memset(s26, 'X', 26);
  s26[26] = '\0';
  TEST_ASSERT_FALSE(qr_v1::encode(s26, out));
  s26[25] = '\0';  // 25 = V1-L alphanumeric capacity itself
  TEST_ASSERT_TRUE(qr_v1::encode(s26, out));
  TEST_ASSERT_FALSE(qr_v1::encode("", out));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(the_loopback_url_matches_the_reference);
  RUN_TEST(the_deployed_url_matches_the_reference);
  RUN_TEST(lowercase_is_refused_not_guessed);
  RUN_TEST(the_capacity_boundary_is_exact);
  return UNITY_END();
}
