// The App's gateway polling against a scripted gateway serving messages from the
// gateway's own encoder (test/wire_fixtures.h).

#include <unity.h>

#include <algorithm>
#include <cstring>
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

class ScriptedGateway final : public Transport {
 public:
  std::deque<std::string> snapshots;
  std::deque<std::string> basemaps;
  std::vector<std::string> requests;
  std::vector<std::string> hosts;
  FakeTime* time = nullptr;
  uint32_t request_ms = 0;
  ReadEnd end_reason = ReadEnd::Closed;
  bool link_pending = false;
  bool live_at_end = false;
  bool alternate_pending = false;
  size_t read_chunk = SIZE_MAX;
  size_t write_chunk = SIZE_MAX;
  size_t available = SIZE_MAX;
  size_t position = 0;
  size_t read_bytes = 0;
  size_t write_bytes = 0;
  unsigned closes = 0;

  bool begin(const Url& url, uint32_t) override {
    position = 0;
    ended_ = ReadEnd::None;
    read_pending_ = false;
    write_pending_ = false;
    if (time != nullptr) {
      time->now_ms += request_ms;
    }
    hosts.emplace_back(url.host.c_str());
    std::deque<std::string>& script = url.path == "/v1/basemap" ? basemaps : snapshots;
    if (script.empty()) {
      return false;
    }
    current_ = script.front();
    if (script.size() > 1) {
      script.pop_front();
    }
    requests.emplace_back();
    return true;
  }
  LinkState advance(uint32_t) override { return link_pending ? LinkState::Pending : LinkState::Ready; }
  IoResult write_some(const uint8_t* data, size_t length) override {
    if (alternate_pending && (write_pending_ = !write_pending_)) {
      return {IoState::Pending, 0};
    }
    const size_t count = std::min(length, write_chunk);
    requests.back().append(reinterpret_cast<const char*>(data), count);
    write_bytes += count;
    return {IoState::Progress, count};
  }
  IoResult read_some(uint8_t* data, size_t capacity) override {
    if (alternate_pending && (read_pending_ = !read_pending_)) {
      return {IoState::Pending, 0};
    }
    if (position >= available) {
      return {IoState::Pending, 0};
    }
    if (position == current_.size()) {
      if (live_at_end) {
        return {IoState::Pending, 0};
      }
      ended_ = end_reason;
      return {end_reason == ReadEnd::Closed ? IoState::End : IoState::Failed, 0};
    }
    const size_t count = std::min({capacity, read_chunk, current_.size() - position, available - position});
    std::memcpy(data, current_.data() + position, count);
    position += count;
    read_bytes += count;
    return {IoState::Progress, count};
  }
  void close() override { ++closes; }
  ReadEnd read_end() const override { return ended_; }

  int count(const char* path) const {
    int n = 0;
    for (const std::string& request : requests) {
      n += request.find(path) != std::string::npos ? 1 : 0;
    }
    return n;
  }

 private:
  std::string current_;
  ReadEnd ended_ = ReadEnd::None;
  bool read_pending_ = false;
  bool write_pending_ = false;
};

void poll_to_completion(App& app) {
  app.tick();
  for (unsigned i = 0; i < 10000 && !app.settled(); ++i) {
    app.tick();
  }
  TEST_ASSERT_TRUE_MESSAGE(app.settled(), "a ready scripted transaction must reach completion");
}

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

  poll_to_completion(app);
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
  poll_to_completion(app);
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
  poll_to_completion(app);
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
    poll_to_completion(app);
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
  poll_to_completion(app);
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
  poll_to_completion(app);
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

// Failed polling backs off while manual refresh bypasses the retry delay.
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

void test_fragmented_transfers_preserve_requests_and_binary_maps_with_a_per_tick_budget() {
  FakeTime time;
  ScriptedGateway gateway;
  gateway.snapshots = {ok_reply(fixtures::kSnapshot, sizeof(fixtures::kSnapshot))};
  gateway.basemaps = {ok_reply(fixtures::kBasemap, sizeof(fixtures::kBasemap))};
  gateway.write_chunk = 3;
  gateway.read_chunk = 7;
  gateway.alternate_pending = true;
  FixedRandom random;
  App app(test_settings(), time, gateway, random);
  TEST_ASSERT_TRUE(app.set_gateway("gateway.test"));

  for (unsigned tick = 0; tick < 10000; ++tick) {
    if (app.status().successes != 0) {
      gateway.read_chunk = SIZE_MAX;
      gateway.alternate_pending = false;
    }
    const size_t before = gateway.read_bytes + gateway.write_bytes;
    app.tick();
    TEST_ASSERT_LESS_OR_EQUAL_UINT(256, gateway.read_bytes + gateway.write_bytes - before);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1000, time.now_ms, "network progress must not advance the clock");
    if (app.settled()) {
      break;
    }
  }
  TEST_ASSERT_TRUE(app.settled());
  TEST_ASSERT_EQUAL_STRING("yellow", banner(app));
  TEST_ASSERT_TRUE(app.basemap().valid());
  TEST_ASSERT_EQUAL_HEX32(fixtures::kBasemapVersion, app.basemap().version());
  TEST_ASSERT_EQUAL(1, gateway.count("/v1/snapshot?n=1a2b3c4d HTTP/1.0\r\n"));
  TEST_ASSERT_EQUAL(1, gateway.count("/v1/basemap?n=1a2b3c4d HTTP/1.0\r\n"));
  TEST_ASSERT_EQUAL_UINT32(2, app.completed_requests());
}

void test_pending_connect_expires_at_two_seconds_and_a_new_source_replaces_it() {
  FakeTime time;
  time.now_ms = UINT32_MAX - 1000;
  ScriptedGateway gateway;
  gateway.snapshots = {ok_reply(fixtures::kSnapshot, sizeof(fixtures::kSnapshot))};
  gateway.link_pending = true;
  FixedRandom random;
  App app(test_settings(), time, gateway, random);
  TEST_ASSERT_TRUE(app.set_gateway("old.test"));
  app.tick();
  time.now_ms += 1999;
  app.tick();
  TEST_ASSERT_EQUAL_UINT32(0, app.status().failures);
  TEST_ASSERT_TRUE(app.fetching());
  ++time.now_ms;
  app.tick();
  TEST_ASSERT_EQUAL_UINT32(1, app.status().failures);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::ConnectFailed), static_cast<int>(app.status().last_outcome));
  TEST_ASSERT_FALSE(app.fetching());

  app.refresh_now();
  app.tick();
  TEST_ASSERT_TRUE(app.fetching());
  const unsigned closes = gateway.closes;
  TEST_ASSERT_TRUE(app.set_gateway("replacement.test"));
  TEST_ASSERT_GREATER_THAN_UINT(closes, gateway.closes);
  gateway.link_pending = false;
  gateway.snapshots = {ok_reply(fixtures::kSnapshotNewBasemap, sizeof(fixtures::kSnapshotNewBasemap))};
  gateway.basemaps.clear();
  poll_to_completion(app);
  Snapshot expected;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WireResult::Ok), static_cast<int>(decode_snapshot(
      fixtures::kSnapshotNewBasemap, sizeof(fixtures::kSnapshotNewBasemap), test_settings().key, fixtures::kNonce, expected)));
  TEST_ASSERT_EQUAL_HEX32(expected.basemap_version, app.snapshot().basemap_version);
  TEST_ASSERT_EQUAL_STRING("replacement.test", gateway.hosts.back().c_str());
  TEST_ASSERT_EQUAL_UINT32(1, app.status().successes);
}

void test_a_live_empty_body_is_pending_until_the_exact_idle_deadline() {
  FakeTime time;
  ScriptedGateway gateway;
  const std::string response = ok_reply(fixtures::kSnapshot, sizeof(fixtures::kSnapshot));
  gateway.snapshots = {response};
  gateway.available = response.find("\r\n\r\n") + 4;
  FixedRandom random;
  App app(test_settings(), time, gateway, random);
  TEST_ASSERT_TRUE(app.set_gateway("gateway.test"));
  for (unsigned i = 0; i < 100 && gateway.position < gateway.available; ++i) {
    app.tick();
  }
  TEST_ASSERT_EQUAL_UINT(gateway.available, gateway.position);
  time.now_ms += 4999;
  app.tick();
  TEST_ASSERT_TRUE(app.fetching());
  TEST_ASSERT_EQUAL_UINT32(0, app.status().failures);
  ++time.now_ms;
  app.tick();
  TEST_ASSERT_FALSE(app.fetching());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Truncated), static_cast<int>(app.status().last_outcome));
  TEST_ASSERT_EQUAL_UINT32(1, app.status().failures);
  TEST_ASSERT_EQUAL_UINT32(kNever, app.since_valid_s());
}

void test_a_body_queued_before_the_idle_deadline_is_drained_at_the_deadline_with_fin() {
  FakeTime time;
  ScriptedGateway gateway;
  const std::string response = ok_reply(fixtures::kSnapshot, sizeof(fixtures::kSnapshot));
  gateway.snapshots = {response};
  gateway.available = response.find("\r\n\r\n") + 4;
  FixedRandom random;
  App app(test_settings(), time, gateway, random);
  TEST_ASSERT_TRUE(app.set_gateway("gateway.test"));
  for (unsigned i = 0; i < 100 && gateway.position < gateway.available; ++i) {
    app.tick();
  }
  TEST_ASSERT_EQUAL_UINT(gateway.available, gateway.position);

  time.now_ms += 4999;
  gateway.available = SIZE_MAX;
  gateway.live_at_end = false;
  ++time.now_ms;
  app.tick();

  TEST_ASSERT_EQUAL_UINT(response.size(), gateway.position);
  TEST_ASSERT_EQUAL_UINT32(1, app.status().successes);
  TEST_ASSERT_EQUAL_UINT32(0, app.status().failures);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::Ok), static_cast<int>(app.status().last_outcome));
  TEST_ASSERT_EQUAL_STRING("yellow", banner(app));
  TEST_ASSERT_EQUAL_UINT32(0, app.since_valid_s());
}

void test_trickled_headers_cannot_extend_the_ten_second_total_deadline() {
  FakeTime time;
  time.now_ms = UINT32_MAX - 5000;
  ScriptedGateway gateway;
  gateway.snapshots = {ok_reply(fixtures::kSnapshot, sizeof(fixtures::kSnapshot))};
  gateway.available = 1;
  FixedRandom random;
  App app(test_settings(), time, gateway, random);
  TEST_ASSERT_TRUE(app.set_gateway("gateway.test"));
  app.tick();
  for (unsigned seconds = 1; seconds < 10; ++seconds) {
    time.advance_s(1);
    gateway.available = gateway.position + 1;
    app.tick();
    TEST_ASSERT_EQUAL_UINT32(0, app.status().failures);
  }
  time.advance_s(1);
  gateway.available = gateway.position + 1;
  app.tick();
  TEST_ASSERT_FALSE(app.fetching());
  TEST_ASSERT_EQUAL_UINT32(1, app.status().failures);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FetchOutcome::BadHead), static_cast<int>(app.status().last_outcome));
  TEST_ASSERT_EQUAL_UINT32(1, app.completed_requests());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_silent_gateway_turns_the_banner_to_no_data_after_a_minute);
  RUN_TEST(test_snapshot_freshness_survives_wrap_extended_silence_and_recovery);
  RUN_TEST(test_a_basemap_is_fetched_once_per_version_and_a_failed_download_leaves_no_map);
  RUN_TEST(test_a_dead_gateway_is_retried_every_ten_seconds_unless_asked);
  RUN_TEST(test_fragmented_transfers_preserve_requests_and_binary_maps_with_a_per_tick_budget);
  RUN_TEST(test_pending_connect_expires_at_two_seconds_and_a_new_source_replaces_it);
  RUN_TEST(test_a_live_empty_body_is_pending_until_the_exact_idle_deadline);
  RUN_TEST(test_a_body_queued_before_the_idle_deadline_is_drained_at_the_deadline_with_fin);
  RUN_TEST(test_trickled_headers_cannot_extend_the_ten_second_total_deadline);
  return UNITY_END();
}
