// Host tests for HTTP chunked framing. See chunked_writer.h.
//
// Asserts the exact bytes on the wire. Chunk framing is length-prefixed, so a
// wrong length or a missing CRLF makes the client reject or hang on the whole
// body -- there is no partial credit.
#include <unity.h>

#include <stdio.h>
#include <string.h>

#include <string>

#include "net/chunked_writer.h"

void setUp() {}
void tearDown() {}

// Records everything written, and can fail on demand.
struct FakeSink {
  std::string out;
  int fail_after = -1;  // -1 = never; otherwise fail the Nth send (0-based)
  int sends = 0;

  bool send(const char* p, size_t n) {
    if (fail_after >= 0 && sends >= fail_after) {
      sends++;
      return false;
    }
    sends++;
    out.append(p, n);
    return true;
  }
};

// Small buffer so the boundary cases are cheap to reach.
using Writer = net::ChunkedWriter<FakeSink, 8>;

static const char* kHead =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
    "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";

static void head_then_one_chunk_then_terminator() {
  FakeSink s;
  Writer w(s);
  TEST_ASSERT_TRUE(w.begin("application/json"));
  TEST_ASSERT_TRUE(w.print("hi"));
  w.end();
  const std::string want = std::string(kHead) + "2\r\nhi\r\n" + "0\r\n\r\n";
  TEST_ASSERT_EQUAL_STRING(want.c_str(), s.out.c_str());
}

static void an_empty_body_still_terminates() {
  FakeSink s;
  Writer w(s);
  w.begin("application/json");
  w.end();
  // No zero-length data chunk -- that would end the body early.
  const std::string want = std::string(kHead) + "0\r\n\r\n";
  TEST_ASSERT_EQUAL_STRING(want.c_str(), s.out.c_str());
}

// The case the buffer exists for: content longer than kBuf must split into
// correctly-sized chunks, not one chunk with a wrong length.
static void content_longer_than_the_buffer_splits_correctly() {
  FakeSink s;
  Writer w(s);
  w.begin("text/csv");
  TEST_ASSERT_TRUE(w.print("ABCDEFGHIJKLMNOPQRSTU"));  // 21 bytes, kBuf = 8
  w.end();
  // 8 + 8 + 5, hex lengths.
  TEST_ASSERT_NOT_NULL(strstr(s.out.c_str(), "8\r\nABCDEFGH\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(s.out.c_str(), "8\r\nIJKLMNOP\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(s.out.c_str(), "5\r\nQRSTU\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(s.out.c_str(), "0\r\n\r\n"));
}

static void an_exact_multiple_of_the_buffer_emits_no_empty_chunk() {
  FakeSink s;
  Writer w(s);
  w.begin("text/csv");
  w.print("ABCDEFGH");  // exactly kBuf
  w.end();
  const std::string want = std::string(
                               "HTTP/1.1 200 OK\r\nContent-Type: text/csv\r\n"
                               "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n") +
                           "8\r\nABCDEFGH\r\n" + "0\r\n\r\n";
  TEST_ASSERT_EQUAL_STRING(want.c_str(), s.out.c_str());
}

// Chunk lengths are HEX. A decimal length is the classic way to produce a body
// that every client rejects.
static void chunk_length_is_hexadecimal() {
  FakeSink s;
  net::ChunkedWriter<FakeSink, 64> w(s);
  w.begin("text/plain");
  w.print("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");  // 30 bytes -> "1e"
  w.end();
  TEST_ASSERT_NOT_NULL(strstr(s.out.c_str(), "1e\r\n"));
  TEST_ASSERT_NULL(strstr(s.out.c_str(), "30\r\n"));
}

static void printf_formats_into_the_body() {
  FakeSink s;
  net::ChunkedWriter<FakeSink, 64> w(s);
  w.begin("application/json");
  TEST_ASSERT_TRUE(w.printf("{\"n\":%d,\"f\":%.1f}", 42, 1.5));
  w.end();
  TEST_ASSERT_NOT_NULL(strstr(s.out.c_str(), "{\"n\":42,\"f\":1.5}"));
}

// Truncation must fail the stream. A clipped number is malformed JSON, and
// silently emitting it is the exact failure this class replaced.
static void printf_truncation_fails_the_stream() {
  FakeSink s;
  net::ChunkedWriter<FakeSink, 64> w(s);
  w.begin("application/json");
  char huge[400];
  memset(huge, 'x', sizeof(huge) - 1);
  huge[sizeof(huge) - 1] = '\0';
  TEST_ASSERT_FALSE(w.printf("%s", huge));
  TEST_ASSERT_FALSE(w.ok());
  // And nothing further is emitted.
  TEST_ASSERT_FALSE(w.print("more"));
}

static void a_dead_peer_stops_everything_after_it() {
  FakeSink s;
  s.fail_after = 1;  // header lands, the first chunk does not
  net::ChunkedWriter<FakeSink, 8> w(s);
  TEST_ASSERT_TRUE(w.begin("application/json"));
  w.print("ABCDEFGHIJ");  // forces a flush
  TEST_ASSERT_FALSE(w.ok());
  TEST_ASSERT_FALSE(w.print("more"));
  w.end();  // must not crash or emit a terminator onto a dead sink
}

static void begin_failure_is_reported_and_sticky() {
  FakeSink s;
  s.fail_after = 0;
  Writer w(s);
  TEST_ASSERT_FALSE(w.begin("application/json"));
  TEST_ASSERT_FALSE(w.ok());
  TEST_ASSERT_FALSE(w.print("anything"));
}

// end() is what the destructor calls, so double-invocation must be harmless --
// otherwise every handler that returns early sends two terminators.
static void end_is_idempotent() {
  FakeSink s;
  Writer w(s);
  w.begin("application/json");
  w.print("hi");
  w.end();
  const size_t after_first = s.out.size();
  w.end();
  w.end();
  TEST_ASSERT_EQUAL_size_t(after_first, s.out.size());
  TEST_ASSERT_TRUE(w.ended());
}

static void extra_headers_are_included() {
  FakeSink s;
  Writer w(s);
  w.begin("text/csv", "Content-Disposition: attachment; filename=x.csv\r\n");
  w.end();
  TEST_ASSERT_NOT_NULL(strstr(s.out.c_str(), "Content-Disposition: attachment; filename=x.csv"));
  // The extra header must sit before the blank line, not after it.
  const char* blank = strstr(s.out.c_str(), "\r\n\r\n");
  const char* disp = strstr(s.out.c_str(), "Content-Disposition");
  TEST_ASSERT_TRUE(disp < blank);
}

static void null_print_is_a_no_op_not_a_crash() {
  FakeSink s;
  Writer w(s);
  w.begin("text/plain");
  TEST_ASSERT_TRUE(w.print(nullptr));
  w.end();
}

// A realistic history body: many small printfs across many buffer boundaries
// must reassemble byte-for-byte.
static void many_small_writes_reassemble_exactly() {
  FakeSink s;
  net::ChunkedWriter<FakeSink, 16> w(s);
  w.begin("application/json");
  std::string expect;
  w.print("[");
  expect += "[";
  for (int i = 0; i < 200; i++) {
    char n[16];
    snprintf(n, sizeof(n), i ? ",%d" : "%d", i);
    w.print(n);
    expect += n;
  }
  w.print("]");
  expect += "]";
  w.end();
  TEST_ASSERT_TRUE(w.ok());
  // Strip the framing and compare the payload.
  std::string body;
  const std::string& raw = s.out;
  size_t pos = raw.find("\r\n\r\n") + 4;
  while (pos < raw.size()) {
    const size_t crlf = raw.find("\r\n", pos);
    const size_t len = strtoul(raw.substr(pos, crlf - pos).c_str(), nullptr, 16);
    if (len == 0)
      break;
    body += raw.substr(crlf + 2, len);
    pos = crlf + 2 + len + 2;
  }
  TEST_ASSERT_EQUAL_STRING(expect.c_str(), body.c_str());
}

// A chunked body is only complete when the zero-length chunk arrives. A
// producer that hits a read error must NOT send it, or the client accepts a
// short file as whole -- for the core dump, a corrupt ELF that decodes to
// nonsense.
static void abort_withholds_the_terminating_chunk() {
  FakeSink s;
  Writer w(s);
  w.begin("application/octet-stream");
  // Long enough to force a flush, so some body really is on the wire -- the
  // realistic case, since the core-dump reader sends 512-byte blocks. Anything
  // still sitting in the buffer is deliberately dropped by abort().
  w.print("ABCDEFGHIJKLMNOP");
  w.abort();
  TEST_ASSERT_NOT_NULL(strstr(s.out.c_str(), "ABCDEFGH"));
  TEST_ASSERT_NULL(strstr(s.out.c_str(), "0\r\n\r\n"));
  TEST_ASSERT_FALSE(w.ok());
}

static void end_after_abort_still_sends_nothing() {
  FakeSink s;
  Writer w(s);
  w.begin("application/octet-stream");
  w.print("partial");
  w.abort();
  const size_t after = s.out.size();
  w.end();  // the destructor will do this too
  TEST_ASSERT_EQUAL_size_t(after, s.out.size());
}

// The terminator is the only thing that marks a chunked body complete. If it
// does not reach the peer the response is unusable, so ok() must not keep
// claiming success -- the same lie abort() exists to prevent.
static void a_failed_terminator_fails_the_stream() {
  FakeSink s;
  // Empty body on purpose, so the ONLY send end() makes is the terminator.
  // With a buffered payload, end() flushes first and a failure there would set
  // ok_ = false on its own -- the test would pass whether or not the terminator
  // result is checked, which is exactly what it must not do.
  s.fail_after = 1;  // send 0 is the header; send 1 is the terminator
  Writer w(s);
  TEST_ASSERT_TRUE(w.begin("application/json"));
  TEST_ASSERT_TRUE(w.ok());
  w.end();
  TEST_ASSERT_FALSE(w.ok());
  TEST_ASSERT_TRUE(w.ended());
}

static void a_successful_terminator_leaves_the_stream_ok() {
  FakeSink s;
  Writer w(s);
  w.begin("application/json");
  w.print("hi");
  w.end();
  TEST_ASSERT_TRUE(w.ok());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(a_failed_terminator_fails_the_stream);
  RUN_TEST(a_successful_terminator_leaves_the_stream_ok);
  RUN_TEST(abort_withholds_the_terminating_chunk);
  RUN_TEST(end_after_abort_still_sends_nothing);
  RUN_TEST(head_then_one_chunk_then_terminator);
  RUN_TEST(an_empty_body_still_terminates);
  RUN_TEST(content_longer_than_the_buffer_splits_correctly);
  RUN_TEST(an_exact_multiple_of_the_buffer_emits_no_empty_chunk);
  RUN_TEST(chunk_length_is_hexadecimal);
  RUN_TEST(printf_formats_into_the_body);
  RUN_TEST(printf_truncation_fails_the_stream);
  RUN_TEST(a_dead_peer_stops_everything_after_it);
  RUN_TEST(begin_failure_is_reported_and_sticky);
  RUN_TEST(end_is_idempotent);
  RUN_TEST(extra_headers_are_included);
  RUN_TEST(null_print_is_a_no_op_not_a_crash);
  RUN_TEST(many_small_writes_reassemble_exactly);
  return UNITY_END();
}
