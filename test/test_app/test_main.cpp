// Fetch scheduling against a scripted server.

#include <unity.h>

#include <string>
#include <vector>

#include "app.h"

using namespace core;

namespace {

class FakeTime final : public TimeSource {
 public:
  uint32_t now_ms = 1000;
  int64_t now_unix = 1794683949;

  uint32_t millis() override { return now_ms; }
  int64_t unix_time() override { return now_unix; }
  bool to_local(int64_t, LocalTime&) override { return false; }

  void advance_s(uint32_t seconds) {
    now_ms += seconds * 1000;
    now_unix += seconds;
  }
};

// Answers each request with the next scripted response; the last one repeats.
class ScriptedServer final : public Transport {
 public:
  explicit ScriptedServer(std::vector<std::string> responses) : responses_(std::move(responses)) {}

  bool connect(const Url&) override { return true; }
  bool write(const char* data, size_t length) override {
    requests.emplace_back(data, length);
    const size_t index = requests.size() - 1 < responses_.size() ? requests.size() - 1 : responses_.size() - 1;
    current_ = responses_[index];
    source_ = MemorySource(current_.data(), current_.size());
    return true;
  }
  ByteSource& input() override { return source_; }
  void close() override {}

  std::vector<std::string> requests;

 private:
  std::vector<std::string> responses_;
  std::string current_;
  MemorySource source_{"", 0};
};

Settings alerts_only() {
  Settings settings;
  settings.threats_url = nullptr;
  settings.alerts_url = "http://mock/v2/iot/active_air_raid_alerts/31.json";
  settings.alerts_token = "token";
  return settings;  // alerts every 20 s, data fresh for 180 s
}

}  // namespace

void setUp() {}
void tearDown() {}

// alerts.in.ua blocks tokens that keep calling after a 429. A rate-limited request
// must not be retried within a minute (the normal failure backoff would be 40 s).
void test_rate_limited_alerts_wait_a_minute_before_retrying() {
  FakeTime time;
  ScriptedServer server({"HTTP/1.0 429 Too Many Requests\r\nContent-Length: 0\r\n\r\n"});
  App app(alerts_only(), time, server);

  app.tick();
  TEST_ASSERT_EQUAL(1, static_cast<int>(server.requests.size()));
  time.advance_s(59);
  app.tick();
  TEST_ASSERT_EQUAL(1, static_cast<int>(server.requests.size()));
  time.advance_s(2);
  app.tick();
  TEST_ASSERT_EQUAL(2, static_cast<int>(server.requests.size()));
}

// alerts.in.ua answers 304 while nothing changes. Those replies confirm the data, so a
// quiet hour must not turn into "NO ALERT DATA".
void test_unchanged_replies_keep_the_alert_fresh() {
  FakeTime time;
  ScriptedServer server({"HTTP/1.0 200 OK\r\nLast-Modified: Sat, 14 Nov 2026 19:00:00 GMT\r\n\r\n\"N\"",
                         "HTTP/1.0 304 Not Modified\r\n\r\n"});
  App app(alerts_only(), time, server);

  for (int i = 0; i < 15; ++i) {  // 300 s of 304 replies, well past the 180 s freshness window
    app.tick();
    time.advance_s(20);
  }
  app.tick();

  ViewModel view;
  app.build_view(view);
  TEST_ASSERT_EQUAL_STRING("none", banner_name(view.banner));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, server.requests.back().find("If-Modified-Since: Sat, 14 Nov 2026"));
}

// Long presses and /debug/refresh ask for an immediate fetch. That must never cut a 429
// wait short or come within alerts.in.ua's 7.5 s minimum of the previous request.
void test_refresh_requests_never_break_the_rate_limits() {
  FakeTime time;
  ScriptedServer server({"HTTP/1.0 429 Too Many Requests\r\nContent-Length: 0\r\n\r\n",
                         "HTTP/1.0 200 OK\r\n\r\n\"N\""});
  App app(alerts_only(), time, server);

  app.tick();  // 429
  time.advance_s(5);
  app.refresh_now();
  app.tick();
  TEST_ASSERT_EQUAL(1, static_cast<int>(server.requests.size()));

  time.advance_s(56);  // the scheduled retry, 61 s after the 429
  app.tick();
  TEST_ASSERT_EQUAL(2, static_cast<int>(server.requests.size()));
  time.advance_s(3);
  app.refresh_now();
  app.tick();
  TEST_ASSERT_EQUAL(2, static_cast<int>(server.requests.size()));
  time.advance_s(5);  // now 8 s after the last request
  app.tick();
  TEST_ASSERT_EQUAL(3, static_cast<int>(server.requests.size()));
}

// Over plain HTTP (the LAN mock server) the alerts token would travel in clear text.
void test_the_real_token_only_travels_over_tls() {
  Settings settings = alerts_only();  // http://mock/...
  settings.alerts_token = "real-secret-token";
  {
    FakeTime time;
    ScriptedServer server({"HTTP/1.0 200 OK\r\n\r\n\"N\""});
    App app(settings, time, server);
    app.tick();
    TEST_ASSERT_EQUAL(1, static_cast<int>(server.requests.size()));
    TEST_ASSERT_EQUAL(std::string::npos, server.requests[0].find("real-secret-token"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, server.requests[0].find("Authorization: Bearer "));
  }
  settings.alerts_url = "https://api.alerts.in.ua/v2/iot/active_air_raid_alerts/31.json";
  {
    FakeTime time;
    ScriptedServer server({"HTTP/1.0 200 OK\r\n\r\n\"N\""});
    App app(settings, time, server);
    app.tick();
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          server.requests[0].find("Authorization: Bearer real-secret-token\r\n"));
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_rate_limited_alerts_wait_a_minute_before_retrying);
  RUN_TEST(test_unchanged_replies_keep_the_alert_fresh);
  RUN_TEST(test_refresh_requests_never_break_the_rate_limits);
  RUN_TEST(test_the_real_token_only_travels_over_tls);
  return UNITY_END();
}
