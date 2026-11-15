#pragma once

#include <cstddef>
#include <cstdint>

#include "alerts.h"
#include "button.h"
#include "http.h"
#include "screen.h"
#include "view.h"
#include "wire.h"

namespace core {

enum class ReadEnd : uint8_t { None, Closed, Timeout, Error };
const char* read_end_name(ReadEnd reason);

enum class LinkState : uint8_t { Pending, Ready, Failed };
enum class IoState : uint8_t { Pending, Progress, End, Failed };
struct IoResult {
  IoState state = IoState::Pending;
  size_t count = 0;
};

// Every operation returns without waiting for network progress.
class Transport {
 public:
  virtual bool begin(const Url& url, uint32_t now) = 0;
  virtual LinkState advance(uint32_t now) = 0;
  virtual IoResult write_some(const uint8_t* data, size_t length) = 0;
  virtual IoResult read_some(uint8_t* data, size_t capacity) = 0;
  virtual void close() = 0;
  virtual ReadEnd read_end() const = 0;

 protected:
  ~Transport() = default;
};

class TimeSource {
 public:
  virtual uint32_t millis() = 0;    // monotonic; wraps after ~49 days
  virtual int64_t unix_time() = 0;  // 0 until the clock is set
  virtual bool to_local(int64_t unix_time, LocalTime& out) = 0;

 protected:
  ~TimeSource() = default;
};

// Unpredictable 32-bit values for request nonces.
class Random {
 public:
  virtual uint32_t next() = 0;

 protected:
  ~Random() = default;
};

struct Settings {
  GatewayKey key = {};
  uint32_t poll_ms = 2000;
  uint32_t retry_ms = 10000;
  uint32_t basemap_retry_ms = 30000;
  uint32_t silent_ms = 60000;      // no valid reply for this long: both sources count as stale
  uint32_t map_request_ms = 30000;  // how long a short press keeps the map up
};

enum class Sound : uint8_t { None, AlertRaised, AlertEscalated, AlertCleared };
const char* sound_name(Sound sound);

enum class FetchOutcome : uint8_t {
  None,
  Ok,
  NotConfigured,
  ConnectFailed,
  RequestFailed,
  BadHead,
  HttpError,
  TooLarge,
  Truncated,
  Rejected,  // MAC, version, type, nonce or structure; see last_rejection
};
const char* fetch_outcome_name(FetchOutcome outcome);

struct TransferStatus {
  FetchOutcome outcome = FetchOutcome::None;
  int16_t http_status = 0;
  WireResult rejection = WireResult::Ok;
  ReadEnd read_end = ReadEnd::None;
  int32_t expected_bytes = -1;
  uint32_t received_bytes = 0;
  uint32_t duration_ms = 0;
};

struct BasemapStatus {
  TransferStatus last;
  TransferStatus last_failure;
  uint32_t successes = 0;
  uint32_t failures = 0;
};

struct GatewayStatus {
  FetchOutcome last_outcome = FetchOutcome::None;
  int last_http_status = 0;
  WireResult last_rejection = WireResult::Ok;
  uint32_t successes = 0;
  uint32_t failures = 0;
  uint32_t rejections = 0;
  bool ever_valid = false;
};

bool valid_gateway_address(const char* address);

class App {
 public:
  App(const Settings& settings, TimeSource& time, Transport& transport, Random& random);

  // Advances at most 256 bytes without waiting for network progress.
  void tick();
  void press(ButtonEvent event);
  // Polls the gateway at the next tick, even while backing off after a failure.
  void refresh_now();
  // "host" or "host:port" (default port 8080). False, and no change, if it is invalid.
  bool set_gateway(const char* address);
  bool gateway_configured() const { return configured_; }
  void build_view(ViewModel& view);
  // The pending sound, cleared by reading it.
  Sound take_sound();

  bool fetching() const { return phase_ != Phase::Idle; }
  bool settled() const { return !fetching() && !forced_ && !basemap_wanted_; }
  uint32_t completed_requests() const { return completed_requests_; }

  const GatewayStatus& status() const { return status_; }
  const BasemapStatus& basemap_status() const { return basemap_status_; }
  const Snapshot& snapshot() const { return snapshot_; }
  const Basemap& basemap() const { return basemap_; }
  // Seconds since the last valid reply; kNever before the first.
  uint32_t since_valid_s();

 private:
  enum class Request : uint8_t { Snapshot, Basemap };
  enum class Phase : uint8_t { Idle, Connect, Write, Head, Body };

  void start(Request request, uint32_t now);
  void progress(uint32_t now);
  void finish(FetchOutcome outcome, uint32_t now);

  uint64_t advance_uptime();
  bool snapshot_recent() const;
  void accept_snapshot();

  Settings settings_;
  TimeSource& time_;
  Transport& transport_;
  Random& random_;
  Url snapshot_url_;
  Url basemap_url_;
  bool configured_ = false;
  bool forced_ = false;
  bool requested_ = false;
  bool basemap_wanted_ = false;
  bool last_poll_failed_ = false;
  bool basemap_failed_ = false;
  uint32_t basemap_failed_at_ = 0;
  uint32_t last_request_ms_ = 0;
  RequestBuffer request_;
  HeadParser head_parser_;
  TransferStatus transfer_;
  Phase phase_ = Phase::Idle;
  Request active_request_ = Request::Snapshot;
  uint32_t nonce_ = 0;
  uint32_t started_ms_ = 0;
  uint32_t progress_ms_ = 0;
  uint32_t completed_requests_ = 0;
  uint16_t written_ = 0;
  GatewayStatus status_;
  BasemapStatus basemap_status_;
  Snapshot snapshot_;
  Basemap basemap_;
  AlertTracker tracker_;
  Sound pending_sound_ = Sound::None;
  bool map_requested_ = false;
  uint32_t map_requested_at_ = 0;
  uint64_t uptime_ms_ = 0;
  uint32_t uptime_millis_ = 0;
  uint64_t valid_uptime_ms_ = 0;
  uint8_t reply_[kMaxSnapshotBytes];
  uint8_t basemap_body_[kMaxBasemapBytes];
};

}  // namespace core
