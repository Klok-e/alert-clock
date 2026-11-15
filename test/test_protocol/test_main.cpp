// Untrusted-input handling: HTTP response heads, request building, timestamps and the
// JSON arena allocator. These run on the PC under AddressSanitizer and UBSan.

#include <unity.h>

#include <cstring>
#include <string>

#include "byte_source.h"
#include "http.h"
#include "json_arena.h"
#include "time_util.h"

using namespace core;

void setUp() {}
void tearDown() {}

// Cloudflare sends header lines longer than the parser's line buffer (cookies,
// report-to). They must be skipped, not fail every fetch.
void test_head_with_overlong_header_line_still_parses() {
  const std::string response = "HTTP/1.1 200 OK\r\n"
                               "Set-Cookie: " + std::string(600, 'x') + "\r\n"
                               "ETag: \"abc123\"\r\n"
                               "Content-Length: 5\r\n"
                               "\r\n"
                               "hello";
  MemorySource in(response.data(), response.size());
  ResponseHead head;

  TEST_ASSERT_EQUAL(static_cast<int>(HeadResult::Ok), static_cast<int>(read_response_head(in, head)));
  TEST_ASSERT_EQUAL(200, head.status);
  TEST_ASSERT_EQUAL_STRING("\"abc123\"", head.etag.c_str());
  TEST_ASSERT_EQUAL(5, head.content_length);
  char body[6] = {};
  TEST_ASSERT_EQUAL(5, static_cast<int>(in.readBytes(body, 5)));
  TEST_ASSERT_EQUAL_STRING("hello", body);
}

// Values a server or a config supplies are echoed into later requests; control
// characters there would let them inject request headers.
void test_control_characters_never_reach_a_request() {
  const std::string response = "HTTP/1.0 200 OK\r\nETag: \"a\rX-Injected: 1\"\r\n\r\n";
  MemorySource in(response.data(), response.size());
  ResponseHead head;
  TEST_ASSERT_EQUAL(static_cast<int>(HeadResult::Ok), static_cast<int>(read_response_head(in, head)));
  TEST_ASSERT_TRUE(head.etag.empty());

  Url url;
  TEST_ASSERT_TRUE(parse_url("https://api.alerts.in.ua/v2/iot/active_air_raid_alerts/31.json", url));
  RequestOptions options;
  options.bearer_token = "token\r\nX-Injected: 1";
  RequestBuffer request;
  TEST_ASSERT_FALSE(build_get_request(url, options, request));
}

void test_rfc3339_offsets_and_fractions_give_the_same_instant() {
  int64_t utc = 0;
  int64_t offset = 0;
  TEST_ASSERT_TRUE(parse_rfc3339("2026-11-14T19:19:09Z", utc));
  TEST_ASSERT_TRUE(parse_rfc3339("2026-11-14T21:19:09.250+02:00", offset));
  TEST_ASSERT_EQUAL_INT64(1794683949, utc);  // calendar.timegm((2026, 11, 14, 19, 19, 9))
  TEST_ASSERT_EQUAL_INT64(utc, offset);

  int64_t untouched = 42;
  TEST_ASSERT_FALSE(parse_rfc3339("2026-02-30T00:00:00Z", untouched));
  TEST_ASSERT_FALSE(parse_rfc3339("2026-11-14T19:19:09", untouched));  // no zone
  TEST_ASSERT_EQUAL_INT64(42, untouched);
}

// ArduinoJson grows strings and pools with reallocate(). Growing a block that is not
// the newest must move it, or it would overwrite its neighbour.
void test_arena_growth_never_overwrites_a_neighbouring_block() {
  ArenaAllocator<128> arena;
  char* first = static_cast<char*>(arena.allocate(16));
  std::memcpy(first, "0123456789abcdef", 16);
  char* second = static_cast<char*>(arena.allocate(16));
  std::memset(second, 'B', 16);

  char* grown = static_cast<char*>(arena.reallocate(first, 32));
  TEST_ASSERT_NOT_NULL(grown);
  TEST_ASSERT_EQUAL_MEMORY("0123456789abcdef", grown, 16);
  std::memset(grown + 16, 'A', 16);
  for (int i = 0; i < 16; ++i) {
    TEST_ASSERT_EQUAL_CHAR('B', second[i]);
  }
  TEST_ASSERT_NULL(arena.allocate(100));  // beyond the remaining space: fails cleanly
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_head_with_overlong_header_line_still_parses);
  RUN_TEST(test_control_characters_never_reach_a_request);
  RUN_TEST(test_rfc3339_offsets_and_fractions_give_the_same_instant);
  RUN_TEST(test_arena_growth_never_overwrites_a_neighbouring_block);
  return UNITY_END();
}
