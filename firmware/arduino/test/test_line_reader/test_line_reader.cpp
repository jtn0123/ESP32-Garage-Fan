// Copyright 2026 Justin
// Native tests for storage/line_reader.h -- the block-buffered line splitter
// behind the SD CSV reader. kBlock is deliberately tiny here so a line that
// straddles a refill boundary costs three characters to reach instead of 512.
#include <unity.h>

#include <cstring>
#include <string>

#include "storage/line_reader.h"

using storage::LineReader;

namespace {

// Minimal byte source over a std::string: the same contract fs::File offers
// the reader (fill a buffer, return the count, <=0 at the end).
class StringSource {
 public:
  explicit StringSource(std::string s) : s_(std::move(s)) {}
  int read(uint8_t* dst, size_t cap) {
    const size_t left = s_.size() - pos_;
    const size_t n = left < cap ? left : cap;
    if (n == 0)
      return 0;
    memcpy(dst, s_.data() + pos_, n);
    pos_ += n;
    return static_cast<int>(n);
  }

 private:
  std::string s_;
  size_t pos_ = 0;
};

}  // namespace

// Unity requires both lifecycle hooks; these tests keep no fixture state,
// so there is nothing to set up or release between cases.
void setUp() {}
void tearDown() {}

static void test_splits_plain_lines() {
  StringSource src("alpha\nbeta\ngamma\n");
  LineReader<StringSource, 4> rd(src);  // 4-byte block: every line refills
  char out[32];
  TEST_ASSERT_TRUE(rd.next(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("alpha", out);
  TEST_ASSERT_TRUE(rd.next(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("beta", out);
  TEST_ASSERT_TRUE(rd.next(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("gamma", out);
  TEST_ASSERT_FALSE(rd.next(out, sizeof(out)));
}

// The bug this reader was written to end: the old character loop emitted a
// line only on seeing '\n', so a file whose last row lacked one lost that row.
static void test_final_line_without_newline_is_kept() {
  StringSource src("first\nlast-no-newline");
  LineReader<StringSource, 8> rd(src);
  char out[32];
  TEST_ASSERT_TRUE(rd.next(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("first", out);
  TEST_ASSERT_TRUE(rd.next(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("last-no-newline", out);
  TEST_ASSERT_FALSE(rd.next(out, sizeof(out)));
}

static void test_crlf_is_stripped() {
  StringSource src("a,1\r\nb,2\r\n");
  LineReader<StringSource, 3> rd(src);
  char out[32];
  TEST_ASSERT_TRUE(rd.next(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("a,1", out);
  TEST_ASSERT_TRUE(rd.next(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("b,2", out);
}

// A real CSV row must survive landing across a block refill unchanged --
// this is the case the block buffer could plausibly get wrong.
static void test_line_spanning_block_boundary() {
  StringSource src("1786336114,19.94,34.2,997.6,-999.0,9\n1786336414,20.01,34.0,997.5,-999.0,9\n");
  LineReader<StringSource, 7> rd(src);
  char out[96];
  TEST_ASSERT_TRUE(rd.next(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("1786336114,19.94,34.2,997.6,-999.0,9", out);
  TEST_ASSERT_TRUE(rd.next(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("1786336414,20.01,34.0,997.5,-999.0,9", out);
  TEST_ASSERT_FALSE(rd.next(out, sizeof(out)));
}

// Truncate rather than split: a truncated row fails the caller's sscanf and
// is skipped, where a split would invent a second row out of the tail.
static void test_overlong_line_truncates_and_does_not_split() {
  StringSource src("0123456789abcdef\nshort\n");
  LineReader<StringSource, 4> rd(src);
  char out[8];
  TEST_ASSERT_TRUE(rd.next(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("0123456", out);  // 7 chars + NUL
  TEST_ASSERT_TRUE(rd.next(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("short", out);
  TEST_ASSERT_FALSE(rd.next(out, sizeof(out)));
}

static void test_empty_source_and_blank_lines() {
  StringSource empty("");
  LineReader<StringSource, 4> rd_empty(empty);
  char out[16];
  TEST_ASSERT_FALSE(rd_empty.next(out, sizeof(out)));

  // A blank line is a real line: it must be reported, not silently swallowed,
  // or the caller's row accounting drifts from the file.
  StringSource blanks("a\n\nb\n");
  LineReader<StringSource, 4> rd(blanks);
  TEST_ASSERT_TRUE(rd.next(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("a", out);
  TEST_ASSERT_TRUE(rd.next(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_TRUE(rd.next(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("b", out);
  TEST_ASSERT_FALSE(rd.next(out, sizeof(out)));
}

static void test_rejects_unusable_output_buffer() {
  StringSource src("anything\n");
  LineReader<StringSource, 4> rd(src);
  char out[4];
  TEST_ASSERT_FALSE(rd.next(nullptr, sizeof(out)));
  TEST_ASSERT_FALSE(rd.next(out, 0));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_splits_plain_lines);
  RUN_TEST(test_final_line_without_newline_is_kept);
  RUN_TEST(test_crlf_is_stripped);
  RUN_TEST(test_line_spanning_block_boundary);
  RUN_TEST(test_overlong_line_truncates_and_does_not_split);
  RUN_TEST(test_empty_source_and_blank_lines);
  RUN_TEST(test_rejects_unusable_output_buffer);
  return UNITY_END();
}
