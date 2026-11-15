#include <unity.h>

#include <string>

#include "http.h"

using namespace core;

void setUp() {}
void tearDown() {}

void test_fragmented_head_skips_an_overlong_header_without_consuming_the_binary_body() {
  const std::string response = "HTTP/1.1 200 OK\r\nSet-Cookie: " + std::string(600, 'x') +
                               "\r\nContent-Length: 5\r\n\r\n" + std::string("\0\xff\r\nX", 5);
  RequestBuffer line;
  HeadParser parser;
  parser.reset(line);
  HeadResult result = HeadResult::Pending;
  size_t consumed = 0;
  while (result == HeadResult::Pending && consumed < response.size()) {
    result = parser.feed(line, static_cast<uint8_t>(response[consumed++]));
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HeadResult::Ok), static_cast<int>(result));
  TEST_ASSERT_EQUAL_INT(200, parser.head().status);
  TEST_ASSERT_EQUAL_INT(5, parser.head().content_length);
  TEST_ASSERT_EQUAL_UINT(5, response.size() - consumed);
  const uint8_t expected[] = {0, 255, 13, 10, 'X'};
  TEST_ASSERT_EQUAL_MEMORY(expected, response.data() + consumed, sizeof(expected));
}

void test_ambiguous_or_unbounded_heads_are_rejected_and_parser_can_restart() {
  const std::string invalid[] = {
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
      "HTTP/1.1 200 OK\r\n" + std::string(4096, 'x'),
  };
  RequestBuffer line;
  HeadParser parser;
  for (const std::string& response : invalid) {
    parser.reset(line);
    HeadResult result = HeadResult::Pending;
    for (const unsigned char byte : response) {
      if (result != HeadResult::Pending) {
        break;
      }
      result = parser.feed(line, byte);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(HeadResult::Malformed), static_cast<int>(result));
  }
  parser.reset(line);
  HeadResult result = HeadResult::Pending;
  for (const unsigned char byte : std::string("HTTP/1.0 404 Missing\r\nContent-Length: 0\r\n\r\n")) {
    result = parser.feed(line, byte);
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HeadResult::Ok), static_cast<int>(result));
  TEST_ASSERT_EQUAL_INT(404, parser.head().status);
  TEST_ASSERT_EQUAL_INT(0, parser.head().content_length);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fragmented_head_skips_an_overlong_header_without_consuming_the_binary_body);
  RUN_TEST(test_ambiguous_or_unbounded_heads_are_rejected_and_parser_can_restart);
  return UNITY_END();
}
