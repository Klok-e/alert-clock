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
  FakeTime* time = nullptr;
  uint32_t request_ms = 0;
  ReadEnd end_reason = ReadEnd::Closed;

  bool connect(const Url& url) override {
    source_.reset("", 0, end_reason);
    if (time != nullptr) {
      time->now_ms += request_ms;
    }
    std::deque<std::string>& script = url.path == "/v1/basemap" ? basemaps : snapshots;
    if (script.empty()) {
      return false;
    }
    current_ = script.front();
    if (script.size() > 1) {
      script.pop_front();
    }
    source_.reset(current_.data(), current_.size(), end_reason);
    return true;
  }
  bool write(const char* data, size_t length) override {
    requests.emplace_back(data, length);
    return true;
  }
  ByteSource& input() override { return source_; }
  void close() override {}
  ReadEnd read_end() const override { return source_.ended; }

  int count(const char* path) const {
    int n = 0;
    for (const std::string& request : requests) {
      n += request.find(path) != std::string::npos ? 1 : 0;
    }
    return n;
  }

 private:
  std::string current_;
  class ScriptedSource final : public ByteSource {
   public:
    ReadEnd ended = ReadEnd::None;

    void reset(const char* data, size_t length, ReadEnd reason) {
      memory_ = MemorySource(data, length);
      reason_ = reason;
      ended = ReadEnd::None;
    }

    int read() override {
      const int byte = memory_.read();
      if (byte < 0) {
        ended = reason_;
      }
      return byte;
    }

   private:
    MemorySource memory_{"", 0};
    ReadEnd reason_ = ReadEnd::Closed;
  } source_;
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

  gateway.snapshots.clear();
  time.now_ms += 59999;
  TEST_ASSERT_EQUAL_STRING_MESSAGE("yellow", banner(app), "valid until the last millisecond before timeout");
  ++time.now_ms;
  TEST_ASSERT_EQUAL_STRING_MESSAGE("nodata", banner(app), "stale at the exact timeout without another tick");
}

void test_snapshot_freshness_survives_wrap_extended_silence_and_recovery() {
  FakeTime time;
  time.now_ms = UINT32_MAX - 30000;
  ScriptedGateway gateway;
  FixedRandom random;
  App app(test_settings(), time, gateway, random);
  TEST_ASSERT_TRUE(app.set_gateway("gateway.test"));
  TEST_ASSERT_EQUAL_STRING_MESSAGE("nodata", banner(app), "no reply must never imply fresh data");
  TEST_ASSERT_EQUAL_UINT32(kNever, app.since_valid_s());

  gateway.snapshots = {ok_reply(fixtures::kSnapshot, sizeof(fixtures::kSnapshot))};
  app.tick();
  TEST_ASSERT_EQUAL_STRING_MESSAGE("yellow", banner(app), "a first good reply restores freshness");
  gateway.snapshots.clear();
  time.now_ms += 59999;
  TEST_ASSERT_EQUAL_STRING_MESSAGE("yellow", banner(app), "timer wrap must not expire a reply early");
  ++time.now_ms;
  TEST_ASSERT_EQUAL_STRING_MESSAGE("nodata", banner(app), "timer wrap must preserve the timeout boundary");

  time.now_ms += 0x80000000u;
  app.tick();
  time.now_ms += 0x80000000u - 60000u;
  app.tick();
  TEST_ASSERT_EQUAL_STRING_MESSAGE("nodata", banner(app), "a full timer period must not revive stale data");
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(UINT32_MAX / 1000u, app.since_valid_s(),
                                 "source age keeps increasing across a full timer period");

  gateway.snapshots = {ok_reply(fixtures::kSnapshot, sizeof(fixtures::kSnapshot))};
  app.refresh_now();
  app.tick();
  TEST_ASSERT_EQUAL_STRING_MESSAGE("yellow", banner(app), "a good reply after extended silence restores freshness");
  TEST_ASSERT_EQUAL_UINT32(0, app.since_valid_s());
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
  gateway.time = &time;
  gateway.request_ms = 37;
  gateway.basemaps = {full.substr(0, full.size() - 100)};  // cut off mid-body
  app.tick();  // the snapshot announces a new version
  time.advance_s(2);
  app.tick();  // the download breaks off
  TEST_ASSERT_EQUAL(2, gateway.count("/v1/basemap"));
  TEST_ASSERT_FALSE(app.basemap().valid());
  const TransferStatus failed = app.basemap_status().last_failure;
  TEST_ASSERT_EQUAL(static_cast<int>(FetchOutcome::Truncated), static_cast<int>(failed.outcome));
  TEST_ASSERT_EQUAL(200, failed.http_status);
  TEST_ASSERT_EQUAL(sizeof(fixtures::kBasemap), failed.expected_bytes);
  TEST_ASSERT_EQUAL(sizeof(fixtures::kBasemap) - 100, failed.received_bytes);
  TEST_ASSERT_EQUAL(static_cast<int>(ReadEnd::Closed), static_cast<int>(failed.read_end));
  TEST_ASSERT_EQUAL(37, failed.duration_ms);
  TEST_ASSERT_EQUAL(1, app.basemap_status().successes);
  TEST_ASSERT_EQUAL(1, app.basemap_status().failures);
  TEST_ASSERT_EQUAL(static_cast<int>(FetchOutcome::Ok), static_cast<int>(app.status().last_outcome));
  TEST_ASSERT_EQUAL(0, app.status().failures);

  gateway.basemaps = {full};
  time.advance_s(31);
  app.tick();  // a later snapshot schedules the basemap retry
  TEST_ASSERT_EQUAL(static_cast<int>(FetchOutcome::Truncated),
                    static_cast<int>(app.basemap_status().last.outcome));
  app.tick();
  TEST_ASSERT_TRUE(app.basemap().valid());
  TEST_ASSERT_EQUAL(3, gateway.count("/v1/basemap"));
  const BasemapStatus& recovered = app.basemap_status();
  TEST_ASSERT_EQUAL(static_cast<int>(FetchOutcome::Ok), static_cast<int>(recovered.last.outcome));
  TEST_ASSERT_EQUAL(sizeof(fixtures::kBasemap), recovered.last.received_bytes);
  TEST_ASSERT_EQUAL(static_cast<int>(ReadEnd::None), static_cast<int>(recovered.last.read_end));
  TEST_ASSERT_EQUAL(2, recovered.successes);
  TEST_ASSERT_EQUAL(1, recovered.failures);
  TEST_ASSERT_EQUAL(static_cast<int>(failed.outcome), static_cast<int>(recovered.last_failure.outcome));
  TEST_ASSERT_EQUAL(failed.http_status, recovered.last_failure.http_status);
  TEST_ASSERT_EQUAL(failed.expected_bytes, recovered.last_failure.expected_bytes);
  TEST_ASSERT_EQUAL(failed.received_bytes, recovered.last_failure.received_bytes);
  TEST_ASSERT_EQUAL(static_cast<int>(failed.read_end), static_cast<int>(recovered.last_failure.read_end));
  TEST_ASSERT_EQUAL(failed.duration_ms, recovered.last_failure.duration_ms);
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
  RUN_TEST(test_snapshot_freshness_survives_wrap_extended_silence_and_recovery);
  RUN_TEST(test_a_basemap_is_fetched_once_per_version_and_a_failed_download_leaves_no_map);
  RUN_TEST(test_a_dead_gateway_is_retried_every_ten_seconds_unless_asked);
  return UNITY_END();
}
