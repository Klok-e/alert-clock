// HTTP response heads from the network. Runs on the PC under AddressSanitizer and UBSan.

#include <unity.h>

#include <string>

#include "byte_source.h"
#include "http.h"

using namespace core;

void setUp() {}
void tearDown() {}

// A header line longer than the parser's line buffer must be skipped, not fail the fetch,
// and the binary body after the head must start exactly where the server put it.
void test_head_with_overlong_header_line_still_parses() {
  const std::string response = "HTTP/1.1 200 OK\r\n"
                               "Set-Cookie: " + std::string(600, 'x') + "\r\n"
                               "Content-Length: 5\r\n"
                               "\r\n"
                               "hello";
  MemorySource in(response.data(), response.size());
  ResponseHead head;

  TEST_ASSERT_EQUAL(static_cast<int>(HeadResult::Ok), static_cast<int>(read_response_head(in, head)));
  TEST_ASSERT_EQUAL(200, head.status);
  TEST_ASSERT_EQUAL(5, head.content_length);
  char body[6] = {};
  TEST_ASSERT_EQUAL(5, static_cast<int>(in.readBytes(body, 5)));
  TEST_ASSERT_EQUAL_STRING("hello", body);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_head_with_overlong_header_line_still_parses);
  return UNITY_END();
}
