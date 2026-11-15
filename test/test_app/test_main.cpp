// The App's gateway polling against a scripted gateway serving messages from the
// gateway's own encoder (test/wire_fixtures.h).

#include <unity.h>

#include <deque>
#include <string>
#include <vector>

#include "../wire_fixtures.h"
#include "app.h"

using namespace core;

namespace {

class FakeTime final : public TimeSource {
 public:
  uint32_t now_ms = 1000;

  uint32_t millis() override { return now_ms; }
  int64_t unix_time() override { return 0; }
  bool to_local(int64_t, LocalTime&) override { return false; }

  void advance_s(uint32_t seconds) { now_ms += seconds * 1000; }
};

// The gateway signs with the nonce the clock sends; the fixtures were signed with this one.
class FixedRandom final : public Random {
 public:
  uint32_t next() override { return fixtures::kNonce; }
};

std::string ok_reply(const uint8_t* body, size_t length) {
  return "HTTP/1.0 200 OK\r\nContent-Length: " + std::to_string(length) + "\r\n\r\n" +
         std::string(reinterpret_cast<const char*>(body), length);
}

// Answers /v1/snapshot and /v1/basemap from separate scripts; each script's last reply
// repeats. An empty script refuses the connection.
class ScriptedGateway final : public Transport {
 public:
  std::deque<std::string> snapshots;
  std::deque<std::string> basemaps;
  std::vector<std::string> requests;

  bool connect(const Url& url) override {
    std::deque<std::string>& script = url.path == "/v1/basemap" ? basemaps : snapshots;
    if (script.empty()) {
      return false;
    }
    current_ = script.front();
    if (script.size() > 1) {
      script.pop_front();
    }
    source_ = MemorySource(current_.data(), current_.size());
    return true;
  }
  bool write(const char* data, size_t length) override {
    requests.emplace_back(data, length);
    return true;
  }
  ByteSource& input() override { return source_; }
  void close() override {}

  int count(const char* path) const {
    int n = 0;
    for (const std::string& request : requests) {
      n += request.find(path) != std::string::npos ? 1 : 0;
    }
    return n;
  }

 private:
  std::string current_;
  MemorySource source_{"", 0};
};

Settings test_settings() {
  Settings settings;
  for (size_t i = 0; i < sizeof(settings.key); ++i) {
    settings.key[i] = static_cast<uint8_t>(i);  // the fixtures' key
  }
  return settings;
}

const char* banner(App& app) {
  ViewModel view;
  app.build_view(view);
  return banner_name(view.banner);
}

}  // namespace

void setUp() {}
void tearDown() {}

// The gateway's stale flags cannot help when the gateway itself is gone: a minute
// without a valid reply must drop the last alert state to "no data", never keep
// showing it as current.
void test_a_silent_gateway_turns_the_banner_to_no_data_after_a_minute() {
  FakeTime time;
  ScriptedGateway gateway;
  gateway.snapshots = {ok_reply(fixtures::kSnapshot, sizeof(fixtures::kSnapshot)), ""};
  gateway.basemaps = {ok_reply(fixtures::kBasemap, sizeof(fixtures::kBasemap))};
  FixedRandom random;
  App app(test_settings(), time, gateway, random);
  TEST_ASSERT_TRUE(app.set_gateway("192.168.1.20"));

  app.tick();
  TEST_ASSERT_EQUAL_STRING("yellow", banner(app));
  TEST_ASSERT_NOT_EQUAL(std::string::npos,
                        gateway.requests[0].find("GET /v1/snapshot?n=1a2b3c4d HTTP/1.0"));

  gateway.snapshots.clear();  // the gateway goes away
  for (int i = 0; i < 29; ++i) {
    time.advance_s(2);
    app.tick();
  }
  TEST_ASSERT_EQUAL_STRING("yellow", banner(app));  // 58 s
  time.advance_s(3);
  app.tick();
  TEST_ASSERT_EQUAL_STRING("nodata", banner(app));  // 61 s
}

// The basemap is downloaded into the buffer it is drawn from. It is fetched once per
// version, and a download that fails must leave no map rather than a half-overwritten one.
void test_a_basemap_is_fetched_once_per_version_and_a_failed_download_leaves_no_map() {
  FakeTime time;
  ScriptedGateway gateway;
  gateway.snapshots = {ok_reply(fixtures::kSnapshot, sizeof(fixtures::kSnapshot))};
  gateway.basemaps = {ok_reply(fixtures::kBasemap, sizeof(fixtures::kBasemap))};
  FixedRandom random;
  App app(test_settings(), time, gateway, random);
  TEST_ASSERT_TRUE(app.set_gateway("192.168.1.20:8080"));

  for (int i = 0; i < 5; ++i) {
    app.tick();
    time.advance_s(2);
  }
  TEST_ASSERT_TRUE(app.basemap().valid());
  TEST_ASSERT_EQUAL_HEX32(fixtures::kBasemapVersion, app.basemap().version());
  TEST_ASSERT_EQUAL(1, gateway.count("/v1/basemap"));

  const std::string full = ok_reply(fixtures::kBasemap, sizeof(fixtures::kBasemap));
  gateway.snapshots = {ok_reply(fixtures::kSnapshotNewBasemap, sizeof(fixtures::kSnapshotNewBasemap))};
  gateway.basemaps = {full.substr(0, full.size() - 100)};  // cut off mid-body
  app.tick();  // the snapshot announces a new version
  time.advance_s(2);
  app.tick();  // the download breaks off
  TEST_ASSERT_EQUAL(2, gateway.count("/v1/basemap"));
  TEST_ASSERT_FALSE(app.basemap().valid());
}

// Each failed poll can block the clock's loop for seconds (connect timeout), so a dead
// gateway is retried every 10 s rather than every 2 s; a long press still polls at once.
void test_a_dead_gateway_is_retried_every_ten_seconds_unless_asked() {
  FakeTime time;
  ScriptedGateway gateway;  // no scripts: every connection is refused
  FixedRandom random;
  App app(test_settings(), time, gateway, random);
  TEST_ASSERT_TRUE(app.set_gateway("192.168.1.20"));
  int attempts = 0;
  auto attempt = [&]() {
    app.tick();
    attempts = static_cast<int>(app.status().failures);
  };

  attempt();
  for (int i = 0; i < 4; ++i) {  // 8 s
    time.advance_s(2);
    attempt();
  }
  TEST_ASSERT_EQUAL(1, attempts);
  time.advance_s(2);  // 10 s
  attempt();
  TEST_ASSERT_EQUAL(2, attempts);
  time.advance_s(1);
  app.press(ButtonEvent::Long);
  attempt();
  TEST_ASSERT_EQUAL(3, attempts);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_silent_gateway_turns_the_banner_to_no_data_after_a_minute);
  RUN_TEST(test_a_basemap_is_fetched_once_per_version_and_a_failed_download_leaves_no_map);
  RUN_TEST(test_a_dead_gateway_is_retried_every_ten_seconds_unless_asked);
  return UNITY_END();
}
